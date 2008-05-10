/*
 * This file is part of DisOrder.
 * Copyright (C) 2004-2008 Richard Kettlewell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */
/** @file server/lookup.c
 * @brief Server lookups
 *
 * To improve performance many server lookups are cached.
 */

#include "disorder-cgi.h"

/** @brief Cached data */
static unsigned flags;

struct queue_entry *dcgi_queue;
struct queue_entry *dcgi_playing;
struct queue_entry *dcgi_recent;

int dcgi_volume_left;
int dcgi_volume_right;

char **dcgi_new;
int dcgi_nnew;

rights_type dcgi_rights;

int dcgi_enabled;
int dcgi_random_enabled;

/** @brief Fetch cachable data */
void dcgi_lookup(unsigned want) {
  unsigned need = want ^ (flags & want);
  struct queue_entry *r, *rnext;
#if 0
  const char *dir, *re;
#endif
  char *rs;

  if(!dcgi_client || !need)
    return;
  if(need & DCGI_QUEUE)
    disorder_queue(dcgi_client, &dcgi_queue);
  if(need & DCGI_PLAYING)
    disorder_playing(dcgi_client, &dcgi_playing);
  if(need & DCGI_NEW)
    disorder_new_tracks(dcgi_client, &dcgi_new, &dcgi_nnew, 0);
  if(need & DCGI_RECENT) {
    /* we need to reverse the order of the list */
    disorder_recent(dcgi_client, &r);
    while(r) {
      rnext = r->next;
      r->next = dcgi_recent;
      dcgi_recent = r;
      r = rnext;
    }
  }
  if(need & DCGI_VOLUME)
    disorder_get_volume(dcgi_client,
                        &dcgi_volume_left, &dcgi_volume_right);
#if 0
  /* DCGI_FILES and DCGI_DIRS are looking obsolete now */
  if(need & (DCGI_FILES|DCGI_DIRS)) {
    if(!(dir = cgi_get("directory")))
      dir = "";
    re = cgi_get("regexp");
    if(need & DCGI_DIRS)
      if(disorder_directories(dcgi_client, dir, re,
                              &dirs, &ndirs))
        ndirs = 0;
    if(need & DCGI_FILES)
      if(disorder_files(dcgi_client, dir, re,
                        &files, &nfiles))
        nfiles = 0;
  }
#endif
  if(need & DCGI_RIGHTS) {
    dcgi_rights = RIGHT_READ;	/* fail-safe */
    if(!disorder_userinfo(dcgi_client, disorder_user(dcgi_client),
                          "rights", &rs))
      parse_rights(rs, &dcgi_rights, 1);
  }
  if(need & DCGI_ENABLED)
    disorder_enabled(dcgi_client, &dcgi_enabled);
  if(need & DCGI_RANDOM_ENABLED)
    disorder_random_enabled(dcgi_client, &dcgi_random_enabled);
  flags |= need;
}

void dcgi_lookup_reset(void) {
  /* Forget everything we knew */
  flags = 0;
  dcgi_recent = 0;
  dcgi_queue = 0;
  dcgi_playing = 0;
  dcgi_rights = 0;
  dcgi_new = 0;
  dcgi_nnew = 0;
  dcgi_enabled = 0;
  dcgi_random_enabled = 0;
  dcgi_volume_left = dcgi_volume_right = 0;
}


/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
