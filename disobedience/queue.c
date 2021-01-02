/*
 * This file is part of DisOrder
 * Copyright (C) 2006-2008 Richard Kettlewell
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
/** @file disobedience/queue.c
 * @brief Disobedience queue widget
 */
#include "disobedience.h"
#include "popup.h"
#include "queue-generic.h"

/** @brief The actual queue */
static struct queue_entry *actual_queue;
static struct queue_entry *actual_playing_track;

/** @brief The playing track */
struct queue_entry *playing_track;

/** @brief When we last got the playing track
 *
 * Set to 0 if the timings are currently off due to having just unpaused.
 */
time_t last_playing;

static void queue_completed(void *v,
                            const char *err,
                            struct queue_entry *q);
static void playing_completed(void *v,
                              const char *err,
                              struct queue_entry *q);

/** @brief Called when either the actual queue or the playing track change */
static void queue_playing_changed(void) {
  /* Check that the playing track isn't in the queue.  There's a race here due
   * to the fact that we issue the two commands at slightly different times.
   * If it goes wrong we re-issue and try again, so that we never offer up an
   * inconsistent state. */
  if(actual_playing_track) {
    struct queue_entry *q;
    for(q = actual_queue; q; q = q->next)
      if(!strcmp(q->id, actual_playing_track->id))
        break;
    if(q) {
      disorder_eclient_playing(client, playing_completed, 0);
      disorder_eclient_queue(client, queue_completed, 0);
      return;
    }
  }
  
  struct queue_entry *q = xmalloc(sizeof *q);
  if(actual_playing_track) {
    *q = *actual_playing_track;
    q->next = actual_queue;
    playing_track = q;
  } else {
    playing_track = NULL;
    q = actual_queue;
  }
  ql_new_queue(&ql_queue, q);
  /* Tell anyone who cares */
  event_raise("queue-list-changed", q);
  event_raise("playing-track-changed", playing_track);
}

/** @brief Update the queue itself */
static void queue_completed(void attribute((unused)) *v,
                            const char *err,
                            struct queue_entry *q) {
  if(err) {
    popup_protocol_error(0, err);
    return;
  }
  actual_queue = q;
  queue_playing_changed();
}

/** @brief Update the playing track */
static void playing_completed(void attribute((unused)) *v,
                              const char *err,
                              struct queue_entry *q) {
  if(err) {
    popup_protocol_error(0, err);
    return;
  }
  actual_playing_track = q;
  queue_playing_changed();
  xtime(&last_playing);
}

/** @brief Schedule an update to the queue
 *
 * Called whenever a track is added to it or removed from it.
 */
static void queue_changed(const char attribute((unused)) *event,
                           void  attribute((unused)) *eventdata,
                           void  attribute((unused)) *callbackdata) {
  D(("queue_changed"));
  gtk_label_set_text(GTK_LABEL(report_label), "updating queue");
  disorder_eclient_queue(client, queue_completed, 0);
}

/** @brief Schedule an update to the playing track
 *
 * Called whenever it changes
 */
static void playing_changed(const char attribute((unused)) *event,
                            void  attribute((unused)) *eventdata,
                            void  attribute((unused)) *callbackdata) {
  D(("playing_changed"));
  gtk_label_set_text(GTK_LABEL(report_label), "updating playing track");
  /* Setting last_playing=0 means that we don't know what the correct value
   * is right now, e.g. because things have been deranged by a pause. */
  last_playing = 0;
  disorder_eclient_playing(client, playing_completed, 0);
}

/** @brief Called regularly
 *
 * Updates the played-so-far field
 */
static gboolean playing_periodic(gpointer attribute((unused)) data) {
  /* If there's a track playing, update its row */
  if(playing_track)
    ql_update_row(playing_track, 0);
  /* If the first (nonplaying) track starts in the past, update the queue to
   * get new expected start times; but rate limit this checking.  (If we only
   * do it once a minute then the rest of the queue can get out of date too
   * easily.) */
  struct queue_entry *q = ql_queue.q;
  if(q && playing_track && !(last_state&(DISORDER_TRACK_PAUSED))) {
    if(q == playing_track)
      q = q->next;
    if(q) {
      time_t now;
      time(&now);
      if(q->expected / 15 < now / 15)
        queue_changed(0,0,0);
    }
  }
  return TRUE;
}

/** @brief Called at startup */
static void queue_init(struct queuelike attribute((unused)) *ql) {
  /* Arrange a callback whenever the playing state changes */ 
  event_register("playing-changed", playing_changed, 0);
  event_register("playing-started", playing_changed, 0);
  /* We reget both playing track and queue at pause/resume so that start times
   * can be computed correctly */
  event_register("pause-changed", playing_changed, 0);
  event_register("pause-changed", queue_changed, 0);
  /* Reget the queue whenever it changes */
  event_register("queue-changed", queue_changed, 0);
  /* ...and once a second anyway */
  g_timeout_add(1000/*ms*/, playing_periodic, 0);
}

static void queue_drop_completed(void attribute((unused)) *v,
                                 const char *err) {
  if(err) {
    popup_protocol_error(0, err);
    return;
  }
  /* The log should tell us the queue changed so we do no more here */
}

/** @brief Called when drag+drop completes */
static void queue_drop(struct queuelike attribute((unused)) *ql,
                       int ntracks,
                       char **tracks, char **ids,
                       struct queue_entry *after_me) {
  int n;

  if(ids) {
    /* Rearrangement */
    if(playing_track) {
      /* If there's a playing track then you can't drag it anywhere  */
      for(n = 0; n < ntracks; ++n) {
        if(!strcmp(playing_track->id, ids[n])) {
          fprintf(stderr, "cannot drag playing track\n");
          return;
        }
      }
      /* You can't tell the server to drag after the playing track by ID, you
       * have to send "". */
      if(after_me == playing_track)
        after_me = NULL;
      /* If you try to drag before the playing track (i.e. after_me=NULL on
       * input) then the effect is just to drag after it, although there's no
       * longer code to explicitly implement this. */
    }
    /* Tell the server to move them.  The log will tell us about the change (if
     * indeed it succeeds!), so no need to rearrange the model now. */
    disorder_eclient_moveafter(client,
                               queue_drop_completed,
                               after_me ? after_me->id : "",
                               (char **)ids, ntracks,
                               NULL);
  } else {
    /* You can't tell the server to insert after the playing track by ID, you
     * have to send "". */
    if(after_me == playing_track)
      after_me = NULL;
    /* Play the tracks */
    disorder_eclient_playafter(client,
                               queue_drop_completed,
                               after_me ? after_me->id : "",
                               (char **)tracks, ntracks,
                               NULL);
  }
}

/** @brief Columns for the queue */
static const struct queue_column queue_columns[] = {
  { "When",   column_when,     0,        COL_RIGHT },
  { "Who",    column_who,      0,        0 },
  { "Artist", column_namepart_dir, "artist", COL_EXPAND|COL_ELLIPSIZE },
  { "Album",  column_namepart_dir, "album",  COL_EXPAND|COL_ELLIPSIZE },
  { "Title",  column_namepart_track, "title",  COL_EXPAND|COL_ELLIPSIZE },
  { "Length", column_length,   0,        COL_RIGHT }
};

/** @brief Pop-up menu for queue */
static struct menuitem queue_menuitems[] = {
  { "Track properties", GTK_STOCK_PROPERTIES, ql_properties_activate, ql_properties_sensitive, 0, 0 },
  { "Select all tracks", GTK_STOCK_SELECT_ALL, ql_selectall_activate, ql_selectall_sensitive, 0, 0 },
  { "Deselect all tracks", NULL, ql_selectnone_activate, ql_selectnone_sensitive, 0, 0 },
  { "Scratch playing track", GTK_STOCK_STOP, ql_scratch_activate, ql_scratch_sensitive, 0, 0 },
  { "Remove track from queue", GTK_STOCK_DELETE, ql_remove_activate, ql_remove_sensitive, 0, 0 },
  { "Adopt track", NULL, ql_adopt_activate, ql_adopt_sensitive, 0, 0 },
};

static const GtkTargetEntry queue_targets[] = {
  {
    QUEUED_TRACKS,                      /* drag type */
    GTK_TARGET_SAME_WIDGET,             /* rearrangement within a widget */
    QUEUED_TRACKS_ID                    /* ID value */
  },
  {
    PLAYABLE_TRACKS,                             /* drag type */
    GTK_TARGET_SAME_APP|GTK_TARGET_OTHER_WIDGET, /* copying between widgets */
    PLAYABLE_TRACKS_ID,                          /* ID value */
  },
  {
    .target = NULL
  }
};

struct queuelike ql_queue = {
  .name = "queue",
  .init = queue_init,
  .columns = queue_columns,
  .ncolumns = sizeof queue_columns / sizeof *queue_columns,
  .menuitems = queue_menuitems,
  .nmenuitems = sizeof queue_menuitems / sizeof *queue_menuitems,
  .drop = queue_drop,
  .drag_source_targets = queue_targets,
  .drag_source_actions = GDK_ACTION_MOVE|GDK_ACTION_COPY,
  .drag_dest_targets = queue_targets,
  .drag_dest_actions = GDK_ACTION_MOVE|GDK_ACTION_COPY,
};

/** @brief Called when a key is pressed in the queue tree view */
static gboolean queue_key_press(GtkWidget attribute((unused)) *widget,
                                GdkEventKey *event,
                                gpointer user_data) {
  /*fprintf(stderr, "queue_key_press type=%d state=%#x keyval=%#x\n",
          event->type, event->state, event->keyval);*/
  switch(event->keyval) {
  case GDK_BackSpace:
  case GDK_Delete:
    if(event->state)
      break;                            /* Only take unmodified DEL/<-- */
    ql_remove_activate(0, user_data);
    return TRUE;                        /* Do not propagate */
  }
  return FALSE;                         /* Propagate */
}

GtkWidget *queue_widget(void) {
  GtkWidget *const w = init_queuelike(&ql_queue);

  /* Catch keypresses */
  g_signal_connect(ql_queue.view, "key-press-event",
                   G_CALLBACK(queue_key_press), &ql_queue);
  return w;
}

/** @brief Return nonzero if @p track is in the queue */
int queued(const char *track) {
  struct queue_entry *q;

  D(("queued %s", track));
  /* Queue will contain resolved name */
  track = namepart_resolve(track);
  for(q = ql_queue.q; q; q = q->next)
    if(!strcmp(q->track, track))
      return 1;
  return 0;
}

/* Playing widget for mini-mode */

static void queue_set_playing_widget(const char attribute((unused)) *event,
                                     void attribute((unused)) *eventdata,
                                     void *callbackdata) {
  GtkLabel *w = callbackdata;

  if(playing_track) {
    const char *artist = namepart(playing_track->track, "display", "artist");
    const char *album = namepart(playing_track->track, "display", "album");
    const char *title = namepart(playing_track->track, "display", "title");
    const char *ldata = column_length(playing_track, NULL);
    if(!ldata)
      ldata = "";
    char *text;
    byte_xasprintf(&text, "%s/%s/%s %s", artist, album, title, ldata);
    gtk_label_set_text(w, text);
  } else
    gtk_label_set_text(w, "");
}

GtkWidget *playing_widget(void) {
  GtkWidget *w = gtk_label_new("");
  gtk_misc_set_alignment(GTK_MISC(w), 1.0, 0);
  /* Spot changes to the playing track */
  event_register("playing-track-changed",
                 queue_set_playing_widget,
                 w);
  /* Use the best-known name for it */
  event_register("lookups-complete",
                 queue_set_playing_widget,
                 w);
  /* Keep the amount played so far up to date */
  event_register("periodic-fast",
                 queue_set_playing_widget,
                 w);
  return frame_widget(w, NULL);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
