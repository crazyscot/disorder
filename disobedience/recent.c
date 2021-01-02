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
/** @file disobedience/recent.c
 * @brief Disobedience recently-played widget
 */
#include "disobedience.h"
#include "popup.h"
#include "queue-generic.h"

/** @brief Update the recently played list */
static void recent_completed(void attribute((unused)) *v,
                             const char *err,
                             struct queue_entry *q) {
  if(err) {
    popup_protocol_error(0, err);
    return;
  }
  /* The recent list is backwards compared to what we wanted */
  struct queue_entry *qr = 0, *qn;
  while(q) {
    qn = q->next;
    /* Swap next/prev pointers */
    q->next = q->prev;
    q->prev = qn;
    /* Remember last node for new head */
    qr = q;
    /* Next node */
    q = qn;
  }
  /* Update the display */
  ql_new_queue(&ql_recent, qr);
  /* Tell anyone who cares */
  event_raise("recent-list-changed", qr);
}

/** @brief Schedule an update to the recently played list
 *
 * Called whenever a track is added to it or removed from it.
 */
static void recent_changed(const char attribute((unused)) *event,
                           void  attribute((unused)) *eventdata,
                           void  attribute((unused)) *callbackdata) {
  D(("recent_changed"));
  gtk_label_set_text(GTK_LABEL(report_label), "updating recently played list");
  disorder_eclient_recent(client, recent_completed, 0);
}

/** @brief Called at startup */
static void recent_init(struct queuelike attribute((unused)) *ql) {
  /* Whenever the recent list changes on the server, re-fetch it */
  event_register("recent-changed", recent_changed, 0);
}

/** @brief Columns for the recently-played list */
static const struct queue_column recent_columns[] = {
  { "When",   column_when,     0,        COL_RIGHT },
  { "Who",    column_who,      0,        0 },
  { "Artist", column_namepart_dir, "artist", COL_EXPAND|COL_ELLIPSIZE },
  { "Album",  column_namepart_dir, "album",  COL_EXPAND|COL_ELLIPSIZE },
  { "Title",  column_namepart_track, "title",  COL_EXPAND|COL_ELLIPSIZE },
  { "Length", column_length,   0,        COL_RIGHT }
};

/** @brief Pop-up menu for recently played list */
static struct menuitem recent_menuitems[] = {
  { "Track properties", GTK_STOCK_PROPERTIES, ql_properties_activate, ql_properties_sensitive,0, 0 },
  { "Play track", GTK_STOCK_MEDIA_PLAY, ql_play_activate, ql_play_sensitive, 0, 0 },
  { "Select all tracks", GTK_STOCK_SELECT_ALL, ql_selectall_activate, ql_selectall_sensitive, 0, 0 },
  { "Deselect all tracks", NULL, ql_selectnone_activate, ql_selectnone_sensitive, 0, 0 },
};

struct queuelike ql_recent = {
  .name = "recent",
  .init = recent_init,
  .columns = recent_columns,
  .ncolumns = sizeof recent_columns / sizeof *recent_columns,
  .menuitems = recent_menuitems,
  .nmenuitems = sizeof recent_menuitems / sizeof *recent_menuitems,
  .drag_source_targets = choose_targets,
  .drag_source_actions = GDK_ACTION_COPY,
};

GtkWidget *recent_widget(void) {
  return init_queuelike(&ql_recent);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
