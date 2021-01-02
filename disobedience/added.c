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
/** @file disobedience/added.c
 * @brief List of recently added tracks
 */
#include "disobedience.h"
#include "popup.h"
#include "queue-generic.h"

/** @brief Called with an updated list of newly-added tracks
 *
 * This is called with a raw list of track names but the rest of @ref
 * disobedience/queue-generic.c requires @ref queue_entry structures
 * with a valid and unique @c id field.  This function fakes it.
 */
static void added_completed(void attribute((unused)) *v,
                            const char *err,
                            int nvec, char **vec) {
  if(err) {
    popup_protocol_error(0, err);
    return;
  }
  /* Convert the vector result to a queue linked list */
  struct queue_entry *q, *qh, *qlast = 0, **qq = &qh;
  int n;
  
  for(n = 0; n < nvec; ++n) {
    q = xmalloc(sizeof *q);
    q->prev = qlast;
    q->track = vec[n];
    q->id = vec[n];             /* unique because a track is only added once */
    *qq = q;
    qq = &q->next;
    qlast = q;
  }
  *qq = 0;
  ql_new_queue(&ql_added, qh);
  /* Tell anyone who cares */
  event_raise("added-list-changed", qh);
}

/** @brief Update the newly-added list */
static void added_changed(const char attribute((unused)) *event,
                          void attribute((unused)) *eventdata,
                          void attribute((unused)) *callbackdata) {
  D(("added_changed"));

  gtk_label_set_text(GTK_LABEL(report_label),
                     "updating newly added track list");
  disorder_eclient_new_tracks(client, added_completed, 0/*all*/, 0);
}

/** @brief Called at startup */
static void added_init(struct queuelike attribute((unused)) *ql) {
  event_register("rescan-complete", added_changed, 0);
}

/** @brief Columns for the new tracks list */
static const struct queue_column added_columns[] = {
  { "Artist", column_namepart_dir, "artist", COL_EXPAND|COL_ELLIPSIZE },
  { "Album",  column_namepart_dir, "album",  COL_EXPAND|COL_ELLIPSIZE },
  { "Title",  column_namepart_track, "title",  COL_EXPAND|COL_ELLIPSIZE },
  { "Length", column_length,   0,        COL_RIGHT }
};

/** @brief Pop-up menu for new tracks list */
static struct menuitem added_menuitems[] = {
  { "Track properties", GTK_STOCK_PROPERTIES, ql_properties_activate, ql_properties_sensitive, 0, 0 },
  { "Play track", GTK_STOCK_MEDIA_PLAY, ql_play_activate, ql_play_sensitive, 0, 0 },
  { "Select all tracks", GTK_STOCK_SELECT_ALL, ql_selectall_activate, ql_selectall_sensitive, 0, 0 },
  { "Deselect all tracks", NULL, ql_selectnone_activate, ql_selectnone_sensitive, 0, 0 },
};

struct queuelike ql_added = {
  .name = "added",
  .init = added_init,
  .columns = added_columns,
  .ncolumns = sizeof added_columns / sizeof *added_columns,
  .menuitems = added_menuitems,
  .nmenuitems = sizeof added_menuitems / sizeof *added_menuitems,
  .drag_source_targets = choose_targets,
  .drag_source_actions = GDK_ACTION_COPY,
};

GtkWidget *added_widget(void) {
  return init_queuelike(&ql_added);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
