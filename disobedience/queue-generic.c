/*
 * This file is part of DisOrder
 * Copyright (C) 2006-2009 Richard Kettlewell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
/** @file disobedience/queue-generic.c
 * @brief Disobedience queue widgets
 *
 * This file provides contains code shared between all the queue-like
 * widgets - the queue, the recent list and the added tracks list.
 *
 * This code is in the process of being rewritten to use the native list
 * widget.
 *
 * There are three @ref queuelike objects: @ref ql_queue, @ref
 * ql_recent and @ref ql_added.  Each has an associated queue linked
 * list and a list store containing the contents.
 *
 * When new contents turn up we rearrange the list store accordingly.
 *
 * NB that while in the server the playing track is not in the queue, in
 * Disobedience, the playing does live in @c ql_queue.q, despite its different
 * status to everything else found in that list.
 *
 * To do:
 * - display playing row in a different color?
 */
#include "disobedience.h"
#include "popup.h"
#include "queue-generic.h"
#include "multidrag.h"
#include "autoscroll.h"

/* Track detail lookup ----------------------------------------------------- */

static void queue_lookups_completed(const char attribute((unused)) *event,
                                   void attribute((unused)) *eventdata,
                                   void *callbackdata) {
  struct queuelike *ql = callbackdata;
  ql_update_list_store(ql);
}

/* Column formatting -------------------------------------------------------- */

/** @brief Format the 'when' column */
const char *column_when(const struct queue_entry *q,
                        const char attribute((unused)) *data) {
  char when[64];
  struct tm tm;
  time_t t;

  D(("column_when"));
  switch(q->state) {
  case playing_isscratch:
  case playing_unplayed:
  case playing_random:
    t = q->expected;
    break;
  case playing_failed:
  case playing_no_player:
  case playing_ok:
  case playing_scratched:
  case playing_started:
  case playing_paused:
  case playing_quitting:
    t = q->played;
    break;
  default:
    t = 0;
    break;
  }
  if(t)
    strftime(when, sizeof when, "%H:%M", localtime_r(&t, &tm));
  else
    when[0] = 0;
  return xstrdup(when);
}

/** @brief Format the 'who' column */
const char *column_who(const struct queue_entry *q,
                       const char attribute((unused)) *data) {
  D(("column_who"));
  return q->submitter ? q->submitter : "";
}

/** @brief Format one of the track name columns */
const char *column_namepart(const struct queue_entry *q,
                            const char *data) {
  D(("column_namepart"));
  return namepart(q->track, "display", data);
}

const char *column_namepart_track(const struct queue_entry *q,
                            const char *data) {
  const char *raw = column_namepart(q, data);
  return trackname_transform("track", raw, "display");
}

const char *column_namepart_dir(const struct queue_entry *q,
                            const char *data) {
  const char *raw = column_namepart(q, data);
  return trackname_transform("dir", raw, "display");
}

/** @brief Format the length column */
const char *column_length(const struct queue_entry *q,
                          const char attribute((unused)) *data) {
  D(("column_length"));
  long l;
  time_t now;
  char *played = 0, *length = 0;

  /* Work out what to say for the length */
  l = namepart_length(q->track);
  if(l > 0)
    byte_xasprintf(&length, "%ld:%02ld", l / 60, l % 60);
  else
    byte_xasprintf(&length, "?:??");
  /* For the currently playing track we want to report how much of the track
   * has been played */
  if(q == playing_track) {
    /* log_state() arranges that we re-get the playing data whenever the
     * pause/resume state changes */
    if(last_state & DISORDER_TRACK_PAUSED)
      l = playing_track->sofar;
    else {
      if(!last_playing)
        return NULL;
      xtime(&now);
      l = playing_track->sofar + (now - last_playing);
    }
    byte_xasprintf(&played, "%ld:%02ld/%s", l / 60, l % 60, length);
    return played;
  } else
    return length;
}

/* List store maintenance -------------------------------------------------- */

/** @brief Return the @ref queue_entry corresponding to @p iter
 * @param model Model that owns @p iter
 * @param iter Tree iterator
 * @return Pointer to queue entry
 */
struct queue_entry *ql_iter_to_q(GtkTreeModel *model,
                                 GtkTreeIter *iter) {
  struct queuelike *ql = g_object_get_data(G_OBJECT(model), "ql");
  GValue v[1];
  memset(v, 0, sizeof v);
  gtk_tree_model_get_value(model, iter, ql->ncolumns + QUEUEPOINTER_COLUMN, v);
  assert(G_VALUE_TYPE(v) == G_TYPE_POINTER);
  struct queue_entry *const q = g_value_get_pointer(v);
  g_value_unset(v);
  return q;
}

/** @brief Return the @ref queue_entry corresponding to @p path
 * @param model Model to query
 * @param path Path into tree
 * @return Pointer to queue entry or NULL
 */
struct queue_entry *ql_path_to_q(GtkTreeModel *model,
                                 GtkTreePath *path) {
  GtkTreeIter iter[1];
  if(!gtk_tree_model_get_iter(model, iter, path))
    return NULL;
  return ql_iter_to_q(model, iter);
}

/** @brief Update one row of a list store
 * @param q Queue entry
 * @param iter Iterator referring to row or NULL to work it out
 */
void ql_update_row(struct queue_entry *q,
                   GtkTreeIter *iter) { 
  const struct queuelike *const ql = q->ql; 

  D(("ql_update_row"));
  /* If no iter was supplied, work it out */
  GtkTreeIter my_iter[1];
  if(!iter) {
    gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store), my_iter);
    struct queue_entry *qq;
    for(qq = ql->q; qq && q != qq; qq = qq->next)
      gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), my_iter);
    if(!qq)
      return;
    iter = my_iter;
  }
  /* Update all the columns */
  for(int col = 0; col < ql->ncolumns; ++col) {
    const char *const v = ql->columns[col].value(q,
                                                 ql->columns[col].data);
    if(v)
      gtk_list_store_set(ql->store, iter,
                         col, v,
                         -1);
  }
  gtk_list_store_set(ql->store, iter,
                     ql->ncolumns + QUEUEPOINTER_COLUMN, q,
                     -1);
  if(q == playing_track)
    gtk_list_store_set(ql->store, iter,
                       ql->ncolumns + BACKGROUND_COLUMN, BG_PLAYING,
                       ql->ncolumns + FOREGROUND_COLUMN, FG_PLAYING,
                       -1);
  else
    gtk_list_store_set(ql->store, iter,
                       ql->ncolumns + BACKGROUND_COLUMN, (char *)0,
                       ql->ncolumns + FOREGROUND_COLUMN, (char *)0,
                       -1);
}

/** @brief Update the list store
 * @param ql Queuelike to update
 *
 * Called when new namepart data is available (and initially).  Doesn't change
 * the rows, just updates the cell values.
 */
void ql_update_list_store(struct queuelike *ql) {
  D(("ql_update_list_store"));
  GtkTreeIter iter[1];
  gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store), iter);
  for(struct queue_entry *q = ql->q; q; q = q->next) {
    ql_update_row(q, iter);
    gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), iter);
  }
}

/** @brief Old and new queue data
 *
 * Used when updating a @ref queuelike with new data from the server.
 */
struct newqueue_data {
  struct queue_entry *old, *new;
};

static void record_queue_map(hash *h,
                             const char *id,
                             struct queue_entry *old,
                             struct queue_entry *new) {
  struct newqueue_data *nqd;

  if(!(nqd = hash_find(h, id))) {
    static const struct newqueue_data empty[1];
    hash_add(h, id, empty, HASH_INSERT);
    nqd = hash_find(h, id);
  }
  if(old) {
#if DEBUG_QUEUE
    fprintf(stderr, " old: %s\n", id);
#endif
    nqd->old = old;
  }
  if(new) {
#if DEBUG_QUEUE
    fprintf(stderr, " new: %s\n", id);
#endif
    nqd->new = new;
  }
}

#if DEBUG_QUEUE
static void dump_queue(struct queue_entry *head, struct queue_entry *mark) {
  for(struct queue_entry *q = head; q; q = q->next) {
    if(q == mark)
      fprintf(stderr, " !");
    fprintf(stderr, " %s\n", q->id);
  }
}

static void dump_rows(struct queuelike *ql) {
  GtkTreeIter iter[1];
  gboolean it = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store),
                                              iter);
  while(it) {
    struct queue_entry *q = ql_iter_to_q(GTK_TREE_MODEL(ql->store), iter);
    it = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), iter);
    fprintf(stderr, " %s\n", q->id);
  }
}
#endif

/** @brief Reset the list store
 * @param ql Queuelike to reset
 * @param newq New queue contents/ordering
 *
 * Updates the queue to match @p newq
 */
void ql_new_queue(struct queuelike *ql,
                  struct queue_entry *newq) {
  D(("ql_new_queue"));
  ++suppress_actions;

  /* Tell every queue entry which queue owns it */
#if DEBUG_QUEUE
  fprintf(stderr, "%s: filling in q->ql\n", ql->name);
#endif
  for(struct queue_entry *q = newq; q; q = q->next)
    q->ql = ql;

#if DEBUG_QUEUE
  fprintf(stderr, "%s: constructing h\n", ql->name);
#endif
  /* Construct map from id to new and old structures */
  hash *h = hash_new(sizeof(struct newqueue_data));
  for(struct queue_entry *q = ql->q; q; q = q->next)
    record_queue_map(h, q->id, q, NULL);
  for(struct queue_entry *q = newq; q; q = q->next)
    record_queue_map(h, q->id, NULL, q);

  /* The easy bit: delete rows not present any more.  In the same pass we
   * update the secret column containing the queue_entry pointer. */
#if DEBUG_QUEUE
  fprintf(stderr, "%s: deleting rows...\n", ql->name);
#endif
  GtkTreeIter iter[1];
  gboolean it = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store),
                                              iter);
  int inserted = 0, deleted = 0, kept = 0;
  while(it) {
    struct queue_entry *q = ql_iter_to_q(GTK_TREE_MODEL(ql->store), iter);
    const struct newqueue_data *nqd = hash_find(h, q->id);
    if(nqd->new) {
      /* Tell this row that it belongs to the new version of the queue */
      gtk_list_store_set(ql->store, iter,
                         ql->ncolumns + QUEUEPOINTER_COLUMN, nqd->new,
                         -1);
      it = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), iter);
      /* We'll need the new start time */
      nqd->new->when = q->when;
      ++kept;
    } else {
      /* Delete this row (and move iter to the next one) */
#if DEBUG_QUEUE
      fprintf(stderr, " delete %s\n", q->id);
#endif
      it = gtk_list_store_remove(ql->store, iter);
      ++deleted;
    }
  }

  /* Now every row's secret column is right, but we might be missing new rows
   * and they might be in the wrong order */

  /* We're going to have to support arbitrary rearrangements, so we might as
   * well add new elements at the end. */
#if DEBUG_QUEUE
  fprintf(stderr, "%s: adding rows...\n", ql->name);
#endif
  struct queue_entry *after = 0;
  for(struct queue_entry *q = newq; q; q = q->next) {
    const struct newqueue_data *nqd = hash_find(h, q->id);
    if(!nqd->old) {
      if(after) {
        /* Try to insert at the right sort of place */
        GtkTreeIter where[1];
        gboolean wit = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store),
                                                     where);
        while(wit && ql_iter_to_q(GTK_TREE_MODEL(ql->store), where) != after)
          wit = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), where);
        if(wit)
          gtk_list_store_insert_after(ql->store, iter, where);
        else
          gtk_list_store_append(ql->store, iter);
      } else
        gtk_list_store_prepend(ql->store, iter);
      gtk_list_store_set(ql->store, iter,
                         ql->ncolumns + QUEUEPOINTER_COLUMN, q,
                         -1);
#if DEBUG_QUEUE
      fprintf(stderr, " add %s\n", q->id);
#endif
      ++inserted;
    }
    after = newq;
  }

  /* Now exactly the right set of rows are present, and they have the right
   * queue_entry pointers in their secret column, but they may be in the wrong
   * order.
   *
   * The current code is simple but amounts to a bubble-sort - we might easily
   * called gtk_tree_model_iter_next a couple of thousand times.
   */
#if DEBUG_QUEUE
  fprintf(stderr, "%s: rearranging rows\n", ql->name);
  fprintf(stderr, "%s: target state:\n", ql->name);
  dump_queue(newq, 0);
  fprintf(stderr, "%s: current state:\n", ql->name);
  dump_rows(ql);
#endif
  it = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ql->store), iter);
  struct queue_entry *tq = newq;        /* t-for-target */
  int swaps = 0, searches = 0;
  int row = 0;
  while(it) {
    struct queue_entry *cq = ql_iter_to_q(GTK_TREE_MODEL(ql->store), iter);
    /* c-for-current */

    /* Everything has the right queue pointer (see above) so it's sufficient to
     * compare pointers to detect mismatches */
    if(cq != tq) {
#if DEBUG_QUEUE
      fprintf(stderr, "  pointer mismatch at row %d\n", row);
      fprintf(stderr, "   target id %s\n", tq->id);
      fprintf(stderr, "   actual id %s\n", cq->id);
#endif
      /* Start looking for the target row fromn the next row */
      GtkTreeIter next[1] = { *iter };
      gboolean nit = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), next);
      while(nit) {
        struct queue_entry *nq = ql_iter_to_q(GTK_TREE_MODEL(ql->store), next);
#if DEBUG_QUEUE
        fprintf(stderr, "   candidate: %s\n", nq->id);
#endif
        if(nq == tq)
          break;
        nit = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), next);
        ++searches;
      }
      /* Note that this assertion will fail in the face of duplicate IDs.
       * q->id really does need to be unique. */
      assert(nit);
      gtk_list_store_swap(ql->store, iter, next);
      *iter = *next;
#if DEBUG_QUEUE
      fprintf(stderr, "%s: found it.  new row state:\n", ql->name);
      dump_rows(ql);
#endif
      ++swaps;
    }
    /* ...and onto the next one */
    it = gtk_tree_model_iter_next(GTK_TREE_MODEL(ql->store), iter);
    tq = tq->next;
    ++row;
  }
#if DEBUG_QUEUE
  fprintf(stderr, "%6s: %3d kept %3d inserted %3d deleted %3d swaps %4d searches\n", ql->name,
          kept, inserted, deleted, swaps, searches);
  fprintf(stderr, "done\n");
#endif
  ql->q = newq;
  /* Set the rest of the columns in new rows */
  ql_update_list_store(ql);
  --suppress_actions;
}

/* Drag and drop ------------------------------------------------------------ */

/** @brief Identify the drop path
 * @param w Destination tree view widget
 * @param model Underlying tree model
 * @param wx X coordinate
 * @param wy Y coordinate
 * @param posp Where to store relative position
 * @return Target path or NULL
 *
 * This is used by ql_drag_motion() and ql_drag_data_received() to identify a
 * drop would or does land.  It's important that they use the same code since
 * otherwise the visual feedback can be inconsistent with the actual effect!
 *
 * Remember to free the returned path.
 */
static GtkTreePath *ql_drop_path(GtkWidget *w,
                                 GtkTreeModel *model,
                                 int wx, int wy,
                                 GtkTreeViewDropPosition *posp) {
  GtkTreePath *path = NULL;
  GtkTreeViewDropPosition pos;
  GtkTreeIter iter[1], last[1];
  int tx, ty;

  gtk_tree_view_convert_widget_to_tree_coords(GTK_TREE_VIEW(w),
                                              wx, wy, &tx, &ty);
  if(gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(w),
                                       wx, wy,
                                       &path,
                                       &pos)) {
    //fprintf(stderr, "gtk_tree_view_get_dest_row_at_pos() -> TRUE\n");
    // Normalize drop position
    switch(pos) {
    case GTK_TREE_VIEW_DROP_INTO_OR_BEFORE:
      pos = GTK_TREE_VIEW_DROP_BEFORE;
      break;
    case GTK_TREE_VIEW_DROP_INTO_OR_AFTER:
      pos = GTK_TREE_VIEW_DROP_AFTER;
      break;
    default: break;
    }
  } else if(gtk_tree_model_get_iter_first(model, iter)) {
    /* If the pointer isn't over any particular row then either it's below
     * the last row, in which case we want the dropzone to be below that row;
     * or it's above the first row (in the column headings) in which case we
     * want the dropzone to be above that row. */
    if(ty >= 0) {
      /* Find the last row */
      do {
        *last = *iter;
      } while(gtk_tree_model_iter_next(model, iter));
      /* The drop target is just after it */
      pos = GTK_TREE_VIEW_DROP_AFTER;
      *iter = *last;
    } else {
      /* The drop target will be just before the first row */
      pos = GTK_TREE_VIEW_DROP_BEFORE;
    }
    path = gtk_tree_model_get_path(model, iter);
  }
  *posp = pos;
  return path;
}

#if 0
static const char *act(GdkDragAction action) {
  struct dynstr d[1];

  dynstr_init(d);
  if(action & GDK_ACTION_DEFAULT)
    dynstr_append_string(d, "|DEFAULT");
  if(action & GDK_ACTION_COPY)
    dynstr_append_string(d, "|COPY");
  if(action & GDK_ACTION_MOVE)
    dynstr_append_string(d, "|MOVE");
  if(action & GDK_ACTION_LINK)
    dynstr_append_string(d, "|LINK");
  if(action & GDK_ACTION_PRIVATE)
    dynstr_append_string(d, "|PRIVATE");
  if(action & GDK_ACTION_ASK)
    dynstr_append_string(d, "|ASK");
  dynstr_terminate(d);
  return d->nvec ? d->vec + 1 : "";
}
#endif

/** @brief Called when a drag moves within a candidate destination
 * @param w Destination widget
 * @param dc Drag context
 * @param x Current pointer location
 * @param y Current pointer location
 * @param time_ Current time
 * @param user_data Pointer to queuelike
 * @return TRUE in a dropzone, otherwise FALSE
 *
 * This is the handler for the "drag-motion" signal.
 */
static gboolean ql_drag_motion(GtkWidget *w,
                               GdkDragContext *dc,
                               gint x,
                               gint y,
                               guint time_,
                               gpointer user_data) {
  struct queuelike *const ql = user_data;
  GdkDragAction action = 0;

  // GTK_DEST_DEFAULT_MOTION vets actions as follows:
  // 1) if dc->suggested_action is in the gtk_drag_dest_set actions
  //    then dc->suggested_action is taken as the action.
  // 2) otherwise if dc->actions intersects the gtk_drag_dest_set actions
  //    then the lowest-numbered member of the intersection is chosen.
  // 3) otherwise no member is chosen and gdk_drag_status() is called
  //    with action=0 to refuse the drop.
  if(dc->suggested_action) {
    if(dc->suggested_action & (GDK_ACTION_MOVE|GDK_ACTION_COPY))
      action = dc->suggested_action;
  } else if(dc->actions & GDK_ACTION_MOVE)
    action = GDK_ACTION_MOVE;
  else if(dc->actions & GDK_ACTION_COPY)
    action = GDK_ACTION_COPY;
  /* TODO this comes up with the wrong answer sometimes.  If we are in the
   * middle of a rearrange then the suggested action will be COPY, which we'll
   * take, even though MOVE would actually be appropriate.  The drag still
   * seems to work, but it _is_ wrong.  The answer is to take the target into
   * account. */
  /*fprintf(stderr, "suggested %s actions %s result %s\n",
          act(dc->suggested_action), act(dc->actions), act(action));*/
  if(action) {
    // If the action is acceptable then we see if this widget is acceptable
    if(gtk_drag_dest_find_target(w, dc, NULL) == GDK_NONE)
      action = 0;
  }
  // Report the status
  //fprintf(stderr, "drag action: %u\n", action);
  gdk_drag_status(dc, action, time_);
  if(action) {
    GtkTreeViewDropPosition pos;

    // Find the drop target
    GtkTreePath *path = ql_drop_path(w, GTK_TREE_MODEL(ql->store), x, y, &pos);
    // Highlight drop target
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), path, pos);
    if(path)
      gtk_tree_path_free(path);
  }
  autoscroll_add(GTK_TREE_VIEW(w));
  return TRUE;                          /* We are (always) in a drop zone */
}

/** @brief Called when a drag moves leaves a candidate destination
 * @param w Destination widget
 * @param dc Drag context
 * @param time_ Current time
 * @param user_data Pointer to queuelike
 *
 * This is the handler for the "drag-leave" signal.
 *
 * It turns out that we get a drag-leave event when the data is dropped, too
 * (See _gtk_drag_dest_handle_event).  This seems logically consistent and is
 * convenient too - for instance it's why autoscroll_remove() gets called at
 * the end of a drag+drop sequence.
 */
static void ql_drag_leave(GtkWidget *w,
                          GdkDragContext attribute((unused)) *dc,
                          guint attribute((unused)) time_,
                          gpointer attribute((unused)) user_data) {
  //struct queuelike *const ql = user_data;

  gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(w), NULL, 0);
  autoscroll_remove(GTK_TREE_VIEW(w));
}

/** @brief Callback to add selected tracks to the selection data
 *
 * Called from ql_drag_data_get().
 */
static void ql_drag_data_get_collect(GtkTreeModel *model,
                                     GtkTreePath attribute((unused)) *path,
                                     GtkTreeIter *iter,
                                     gpointer data) {
  struct dynstr *const result = data;
  struct queue_entry *const q = ql_iter_to_q(model, iter);

  dynstr_append_string(result, q->id);
  dynstr_append(result, '\n');
  dynstr_append_string(result, q->track);
  dynstr_append(result, '\n');
}

/** @brief Called to extract the dragged data from the source queuelike
 * @param w Source widget (the tree view)
 * @param dc Drag context
 * @param data Where to put the answer
 * @param info Target @c info parameter
 * @param time_ Time data requested (for some reason not a @c time_t)
 * @param user_data The queuelike
 *
 * The list of tracks is converted into a single string, consisting of IDs
 * and track names.  Each is terminated by a newline.  Including both ID and
 * track name means that the receiver can use whichever happens to be more
 * convenient.
 *
 * If there are no IDs for rows in this widget then the ID half is undefined.
 *
 * This is the handler for the "drag-data-get" signal.
 */
static void ql_drag_data_get(GtkWidget attribute((unused)) *w,
                             GdkDragContext attribute((unused)) *dc,
                             GtkSelectionData *data,
                             guint attribute((unused)) info,
                             guint attribute((unused)) time_,
                             gpointer user_data) {
  struct queuelike *const ql = user_data;
  struct dynstr result[1];

  //fprintf(stderr, "ql_drag_data_get %s info=%d\n", ql->name, info);
  dynstr_init(result);
  gtk_tree_selection_selected_foreach(ql->selection,
                                      ql_drag_data_get_collect,
                                      result);
  // TODO must not be able to drag playing track!
  //fprintf(stderr, "drag-data-get: %.*s\n",
  //        result->nvec, result->vec);
  /* gtk_selection_data_set_text() insists that data->target is one of a
   * variety of stringy atoms.  TODO: where does this value actually come
   * from?  */
  gtk_selection_data_set(data,
                         GDK_TARGET_STRING,
                         8, (guchar *)result->vec, result->nvec);
}

/** @brief Called when drag data is received
 * @param w Target widget (the tree view)
 * @param dc Drag context
 * @param x The drop location
 * @param y The drop location
 * @param data The selection data
 * @param info_ The target type that was chosen
 * @param time_ Time data received (for some reason not a @c time_t)
 * @param user_data The queuelike
 *
 * This is the handler for the "drag-data-received" signal.
 */
static void ql_drag_data_received(GtkWidget attribute((unused)) *w,
                                  GdkDragContext attribute((unused)) *dc,
                                  gint x,
                                  gint y,
                                  GtkSelectionData *data,
                                  guint info_,
                                  guint attribute((unused)) time_,
                                  gpointer user_data) {
  struct queuelike *const ql = user_data;
  char *result, *p;
  struct vector ids[1], tracks[1];
  int parity = 0;

  //fprintf(stderr, "drag-data-received: %d,%d info=%u\n", x, y, info_);
  /* Get the selection string */
  p = result = (char *)gtk_selection_data_get_text(data);
  if(!result) {
    //fprintf(stderr, "gtk_selection_data_get_text() returned NULL\n");
    return;
  }
  //fprintf(stderr, "%s--\n", result);
  /* Parse it back into IDs and track names */
  vector_init(ids);
  vector_init(tracks);
  while(*p) {
    char *nl = strchr(p, '\n');
    if(!nl)
      break;
    *nl = 0;
    //fprintf(stderr, "  %s\n", p);
    vector_append(parity++ & 1 ? tracks : ids, xstrdup(p));
    p = nl + 1;
  }
  g_free(result);
  if(ids->nvec != tracks->nvec) {
    //fprintf(stderr, "  inconsistent drag data!\n");
    return;
  }
  vector_terminate(ids);
  vector_terminate(tracks);
  /* Figure out which row the drop precedes (if any) */
  GtkTreeViewDropPosition pos;
  struct queue_entry *q;
  GtkTreePath *path = ql_drop_path(w, GTK_TREE_MODEL(ql->store), x, y, &pos);
  if(path) {
    q = ql_path_to_q(GTK_TREE_MODEL(ql->store), path);
    //fprintf(stderr, "  drop path: %s q=%p pos=%d\n",
    //        gtk_tree_path_to_string(path), q, pos);
  } else {
    /* This generally means a drop past the end of the queue.  We find the last
     * element in the queue and ask to move after that. */
    for(q = ql->q; q && q->next; q = q->next)
      ;
    //fprintf(stderr, "  after end.  q=%p.  pos=%d\n", q, pos);
  }
  switch(pos) {
  case GTK_TREE_VIEW_DROP_BEFORE:
  case GTK_TREE_VIEW_DROP_INTO_OR_BEFORE:
    if(q) {
      q = q->prev;
      //fprintf(stderr, "  but we like to drop near %s\n",
      //        q ? q->id : "NULL");
    }
    break;
  default:
    break;
  }
  /* Guarantee we never drop an empty list */
  if(!tracks->nvec)
    return;
  /* Note that q->id can match one of ids[].  This doesn't matter for
   * moveafter but TODO may matter for playlist support. */
  switch(info_) {
  case QUEUED_TRACKS_ID:
  case PLAYLIST_TRACKS_ID:
    /* Rearrangement within some widget.  Send ID and track data. */
    ql->drop(ql, tracks->nvec, tracks->vec, ids->vec, q);
    break;
  case PLAYABLE_TRACKS_ID:
    /* Copying between widgets.  IDs mean nothing so don't send them. */
    ql->drop(ql, tracks->nvec, tracks->vec, NULL, q);
    break;
  }
  if(path)
    gtk_tree_path_free(path);
}

static int count_drag_targets(const GtkTargetEntry *targets) {
  const GtkTargetEntry *t = targets;

  while(t->target)
    ++t;
  return t - targets;
}

/** @brief Initialize a @ref queuelike */
GtkWidget *init_queuelike(struct queuelike *ql) {
  D(("init_queuelike"));
  /* Create the list store.  We add an extra column to hold a pointer to the
   * queue_entry. */
  GType *types = xcalloc(ql->ncolumns + EXTRA_COLUMNS, sizeof (GType));
  for(int n = 0; n < ql->ncolumns + EXTRA_COLUMNS; ++n)
    types[n] = G_TYPE_STRING;
  types[ql->ncolumns + QUEUEPOINTER_COLUMN] = G_TYPE_POINTER;
  ql->store = gtk_list_store_newv(ql->ncolumns + EXTRA_COLUMNS, types);
  g_object_set_data(G_OBJECT(ql->store), "ql", (void *)ql);

  /* Create the view */
  ql->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ql->store));
  gtk_tree_view_set_rules_hint(GTK_TREE_VIEW(ql->view), TRUE);

  /* Create cell renderers and label columns */
  for(int n = 0; n < ql->ncolumns; ++n) {
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    if(ql->columns[n].flags & COL_ELLIPSIZE)
      g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, (char *)0);
    if(ql->columns[n].flags & COL_RIGHT)
      g_object_set(r, "xalign", (gfloat)1.0, (char *)0);
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes
      (ql->columns[n].name,
       r,
       "text", n,
       "cell-background", ql->ncolumns + BACKGROUND_COLUMN,
       "foreground", ql->ncolumns + FOREGROUND_COLUMN,
       (char *)0);
    gtk_tree_view_column_set_resizable(c, TRUE);
    gtk_tree_view_column_set_reorderable(c, TRUE);
    if(ql->columns[n].flags & COL_EXPAND)
      g_object_set(c, "expand", TRUE, (char *)0);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ql->view), c);
  }

  /* The selection should support multiple things being selected */
  ql->selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(ql->view));
  g_object_ref(ql->selection);
  gtk_tree_selection_set_mode(ql->selection, GTK_SELECTION_MULTIPLE);

  /* Catch button presses */
  g_signal_connect(ql->view, "button-press-event",
                   G_CALLBACK(ql_button_release), ql);

  /* Drag+drop*/
  if(ql->drop) {
    /* Originally this was:
     *
     *   gtk_tree_view_set_reorderable(GTK_TREE_VIEW(ql->view), TRUE);
     *
     * However this has a two deficiencies:
     *
     *   1) Only one row can be dragged at once.  It would be nice
     *      to be able to do bulk rearrangements since the server
     *      can cope with that well.
     *   2) Dragging between windows is not possible.  When playlist
     *      support appears, it should be possible to drag tracks
     *      from the choose tag into the playlist.
     *
     * At the time of writing neither of these problems are fully solved, the
     * code as it stands is just a stepping stone in that direction.
     */

    /* This view will act as a drag source */
    gtk_drag_source_set(ql->view,
                        GDK_BUTTON1_MASK,
                        ql->drag_source_targets,
                        count_drag_targets(ql->drag_source_targets),
                        ql->drag_dest_actions);
    /* This view will act as a drag destination */
    gtk_drag_dest_set(ql->view,
                      GTK_DEST_DEFAULT_HIGHLIGHT|GTK_DEST_DEFAULT_DROP,
                      ql->drag_dest_targets,
                      count_drag_targets(ql->drag_dest_targets),
                      ql->drag_dest_actions);
    g_signal_connect(ql->view, "drag-motion",
                     G_CALLBACK(ql_drag_motion), ql);
    g_signal_connect(ql->view, "drag-leave",
                     G_CALLBACK(ql_drag_leave), ql);
    g_signal_connect(ql->view, "drag-data-get",
                     G_CALLBACK(ql_drag_data_get), ql);
    g_signal_connect(ql->view, "drag-data-received",
                     G_CALLBACK(ql_drag_data_received), ql);
    make_treeview_multidrag(ql->view, NULL);
    // TODO playing track should be refused by predicate arg
  } else {
    /* For queues that cannot accept a drop we still accept a copy out */
    gtk_drag_source_set(ql->view,
                        GDK_BUTTON1_MASK,
                        ql->drag_source_targets,
                        count_drag_targets(ql->drag_source_targets),
                        ql->drag_source_actions);
    g_signal_connect(ql->view, "drag-data-get",
                     G_CALLBACK(ql_drag_data_get), ql);
    make_treeview_multidrag(ql->view, NULL);
  }
  
  /* TODO style? */

  if(ql->init)
    ql->init(ql);

  /* Update display text when lookups complete */
  event_register("lookups-completed", queue_lookups_completed, ql);
  
  GtkWidget *scrolled = scroll_widget(ql->view);
  g_object_set_data(G_OBJECT(scrolled), "type", (void *)ql_tabtype(ql));
  return scrolled;
}

/** @brief Destroy a queuelike
 * @param ql Queuelike to destroy
 *
 * Returns @p ql to its initial state.
 */
void destroy_queuelike(struct queuelike *ql) {
  if(ql->store) {
    g_object_unref(ql->store);
    ql->store = NULL;
  }
  if(ql->view) {
    gtk_object_destroy(GTK_OBJECT(ql->view));
    ql->view = NULL;
  }
  if(ql->menu) {
    gtk_object_destroy(GTK_OBJECT(ql->menu));
    ql->menu = NULL;
  }
  if(ql->selection) {
    g_object_unref(ql->selection);
    ql->selection = NULL;
  }
  ql->q = NULL;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
