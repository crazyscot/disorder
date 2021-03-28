/*
 * This file is part of DisOrder.
 * Copyright (C) 2004-2012 Richard Kettlewell
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

#include "disorder-server.h"
#include "basen.h"

#ifndef NONCE_SIZE
# define NONCE_SIZE 16
#endif

#ifndef CONFIRM_SIZE
/** @brief Size of nonce in confirmation string in 32-bit words
 *
 * 64 bits gives 11 digits (in base 62).
 */
# define CONFIRM_SIZE 2
#endif

int volume_left, volume_right;		/* last known volume */

/** @brief Accept all well-formed login attempts
 *
 * Used in debugging.
 */
int wideopen;

struct listener {
  const char *name;
  int pf;
  int privileged;
};

struct conn;

/** @brief Signature for line reader callback
 * @param c Connection
 * @param line Line
 * @return 0 if incomplete, 1 if complete
 *
 * @p line is 0-terminated and excludes the newline.  It points into the
 * input buffer so will become invalid shortly.
 */
typedef int line_reader_type(struct conn *c,
                             char *line);

/** @brief Signature for with-body command callbacks
 * @param c Connection
 * @param body List of body lines
 * @param nbody Number of body lines
 * @param u As passed to fetch_body()
 * @return 0 to suspend input, 1 if complete
 *
 * The body strings are allocated (so survive indefinitely) and don't include
 * newlines.
 */
typedef int body_callback_type(struct conn *c,
                               char **body,
                               int nbody,
                               void *u);

/** @brief One client connection */
struct conn {
  /** @brief Read commands from here */
  ev_reader *r;
  /** @brief Send responses to here */
  ev_writer *w;
  /** @brief Underlying file descriptor */
  int fd;
  /** @brief Unique identifier for connection used in log messages */
  unsigned tag;
  /** @brief Login name or NULL */
  char *who;
  /** @brief Event loop */
  ev_source *ev;
  /** @brief Nonce chosen for this connection */
  unsigned char nonce[NONCE_SIZE];
  /** @brief Current reader callback
   *
   * We change this depending on whether we're servicing the @b log command
   */
  ev_reader_callback *reader;
  /** @brief Event log output sending to this connection */
  struct eventlog_output *lo;
  /** @brief Parent listener */
  const struct listener *l;
  /** @brief Login cookie or NULL */
  char *cookie;
  /** @brief Connection rights */
  rights_type rights;
  /** @brief Next connection */
  struct conn *next;
  /** @brief True if pending rescan had 'wait' set */
  int rescan_wait;
  /** @brief Playlist that this connection locks */
  const char *locked_playlist;
  /** @brief When that playlist was locked */
  time_t locked_when;
  /** @brief Line reader function */
  line_reader_type *line_reader;
  /** @brief Called when command body has been read */
  body_callback_type *body_callback;
  /** @brief Passed to @c body_callback */
  void *body_u;
  /** @brief Accumulating body */
  struct vector body[1];

  /** @brief Nonzero if an active RTP request exists */
  int rtp_requested;

  /** @brief RTP destination (if @ref rtp_requested is nonzero) */
  struct sockaddr_storage rtp_destination;
};

/** @brief Linked list of connections */
static struct conn *connections;

static int reader_callback(ev_source *ev,
			   ev_reader *reader,
			   void *ptr,
			   size_t bytes,
			   int eof,
			   void *u);
static int c_playlist_set_body(struct conn *c,
                               char **body,
                               int nbody,
                               void *u);
static int fetch_body(struct conn *c,
                      body_callback_type body_callback,
                      void *u);
static int body_line(struct conn *c, char *line);
static int command(struct conn *c, char *line);

static const char *noyes[] = { "no", "yes" };

/** @brief Remove a connection from the connection list
 *
 * This is a good place for cleaning things up when connections are closed for
 * any reason.
 */
static void remove_connection(struct conn *c) {
  struct conn **cc;

  if(c->rtp_requested) {
    rtp_request_cancel(&c->rtp_destination);
    c->rtp_requested = 0;
  }
  for(cc = &connections; *cc && *cc != c; cc = &(*cc)->next)
    ;
  if(*cc)
    *cc = c->next;
}

/** @brief Called when a connection's writer fails or is shut down
 *
 * If the connection still has a raeder that is cancelled.
 */
static int writer_error(ev_source attribute((unused)) *ev,
			int errno_value,
			void *u) {
  struct conn *c = u;

  D(("server writer_error S%x %d", c->tag, errno_value));
  if(errno_value == 0) {
    /* writer is done */
    D(("S%x writer completed", c->tag));
  } else {
    if(errno_value != EPIPE)
      disorder_error(errno_value, "S%x write error on socket", c->tag);
    if(c->r) {
      D(("cancel reader"));
      ev_reader_cancel(c->r);
      c->r = 0;
    }
    D(("done cancel reader"));
  }
  c->w = 0;
  ev_report(ev);
  remove_connection(c);
  return 0;
}

/** @brief Called when a conncetion's reader fails or is shut down
 *
 * If connection still has a writer then it is closed.
 */
static int reader_error(ev_source attribute((unused)) *ev,
			int errno_value,
			void *u) {
  struct conn *c = u;

  D(("server reader_error S%x %d", c->tag, errno_value));
  disorder_error(errno_value, "S%x read error on socket", c->tag);
  if(c->w)
    ev_writer_close(c->w);
  c->w = 0;
  c->r = 0;
  ev_report(ev);
  remove_connection(c);
  return 0;
}

static int c_disable(struct conn *c, char **vec, int nvec) {
  if(nvec == 0)
    disable_playing(c->who, c->ev);
  else if(nvec == 1 && !strcmp(vec[0], "now"))
    disable_playing(c->who, c->ev);
  else {
    sink_writes(ev_writer_sink(c->w), "550 invalid argument\n");
    return 1;			/* completed */
  }
  sink_writes(ev_writer_sink(c->w), "250 OK\n");
  return 1;			/* completed */
}

static int c_enable(struct conn *c,
		    char attribute((unused)) **vec,
		    int attribute((unused)) nvec) {
  enable_playing(c->who, c->ev);
  /* Enable implicitly unpauses if there is nothing playing */
  if(paused && !playing) resume_playing(c->who);
  sink_writes(ev_writer_sink(c->w), "250 OK\n");
  return 1;			/* completed */
}

static int c_enabled(struct conn *c,
		     char attribute((unused)) **vec,
		     int attribute((unused)) nvec) {
  sink_printf(ev_writer_sink(c->w), "252 %s\n", noyes[playing_is_enabled()]);
  return 1;			/* completed */
}

static int c_play(struct conn *c, char **vec,
		  int attribute((unused)) nvec) {
  const char *track;
  struct queue_entry *q;
  
  if(!trackdb_exists(vec[0])) {
    sink_writes(ev_writer_sink(c->w), "550 track is not in database\n");
    return 1;
  }
  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  q = queue_add(track, c->who, WHERE_BEFORE_RANDOM, NULL, origin_picked);
  queue_write();
  sink_printf(ev_writer_sink(c->w), "252 %s\n", q->id);
  /* We make sure the track at the head of the queue is prepared, just in case
   * we added it.  We could be more subtle but prepare() will ensure we don't
   * prepare the same track twice so there's no point. */
  if(qhead.next != &qhead)
    prepare(c->ev, qhead.next);
  /* If the queue was empty but we are for some reason paused then
   * unpause. */
  if(!playing) resume_playing(0);
  play(c->ev);
  return 1;			/* completed */
}

static int c_playafter(struct conn *c, char **vec,
		  int attribute((unused)) nvec) {
  const char *track;
  struct queue_entry *q;
  const char *afterme = vec[0];

  for(int n = 1; n < nvec; ++n) {
    if(!trackdb_exists(vec[n])) {
      sink_writes(ev_writer_sink(c->w), "550 track is not in database\n");
      return 1;
    }
    if(!(track = trackdb_resolve(vec[n]))) {
      sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
      return 1;
    }
    q = queue_add(track, c->who, WHERE_AFTER, afterme, origin_picked);
    if(!q) {
      sink_printf(ev_writer_sink(c->w), "550 No such ID\n");
      return 1;
    }
    disorder_info("added %s as %s after %s", track, q->id, afterme);
    afterme = q->id;
  }
  queue_write();
  sink_printf(ev_writer_sink(c->w), "252 OK\n");
  /* We make sure the track at the head of the queue is prepared, just in case
   * we added it.  We could be more subtle but prepare() will ensure we don't
   * prepare the same track twice so there's no point. */
  if(qhead.next != &qhead) {
    prepare(c->ev, qhead.next);
    disorder_info("prepared %s", qhead.next->id);
  }
  /* If the queue was empty but we are for some reason paused then
   * unpause. */
  if(!playing)
    resume_playing(0);
  play(c->ev);
  return 1;			/* completed */
}

static int c_remove(struct conn *c, char **vec,
		    int attribute((unused)) nvec) {
  struct queue_entry *q;

  if(!(q = queue_find(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 no such track on the queue\n");
    return 1;
  }
  if(!right_removable(c->rights, c->who, q)) {
    disorder_error(0, "%s attempted remove but lacks required rights", c->who);
    sink_writes(ev_writer_sink(c->w),
		"510 Not authorized to remove that track\n");
    return 1;
  }
  queue_remove(q, c->who);
  /* De-prepare the track. */
  abandon(c->ev, q);
  /* See about adding a new random track */
  add_random_track(c->ev);
  /* Prepare whatever the next head track is. */
  if(qhead.next != &qhead)
    prepare(c->ev, qhead.next);
  queue_write();
  sink_writes(ev_writer_sink(c->w), "250 removed\n");
  return 1;			/* completed */
}

static int c_scratch(struct conn *c,
		     char **vec,
		     int nvec) {
  if(!playing) {
    sink_writes(ev_writer_sink(c->w), "250 nothing is playing\n");
    return 1;			/* completed */
  }
  /* TODO there is a bug here: if we specify an ID but it's not the currently
   * playing track then you will get 550 if you weren't authorized to scratch
   * the currently playing track. */
  if(!right_scratchable(c->rights, c->who, playing)) {
    disorder_error(0, "%s attempted scratch but lacks required rights", c->who);
    sink_writes(ev_writer_sink(c->w),
		"510 Not authorized to scratch that track\n");
    return 1;
  }
  scratch(c->who, nvec == 1 ? vec[0] : 0);
  /* If you scratch an unpaused track then it is automatically unpaused */
  resume_playing(0);
  sink_writes(ev_writer_sink(c->w), "250 scratched\n");
  return 1;			/* completed */
}

static int c_pause(struct conn *c,
		   char attribute((unused)) **vec,
		   int attribute((unused)) nvec) {
  if(!playing) {
    sink_writes(ev_writer_sink(c->w), "250 nothing is playing\n");
    return 1;			/* completed */
  }
  if(paused) {
    sink_writes(ev_writer_sink(c->w), "250 already paused\n");
    return 1;			/* completed */
  }
  if(pause_playing(c->who) < 0)
    sink_writes(ev_writer_sink(c->w), "550 cannot pause this track\n");
  else
    sink_writes(ev_writer_sink(c->w), "250 paused\n");
  return 1;
}

static int c_resume(struct conn *c,
		   char attribute((unused)) **vec,
		   int attribute((unused)) nvec) {
  if(!paused) {
    sink_writes(ev_writer_sink(c->w), "250 not paused\n");
    return 1;			/* completed */
  }
  resume_playing(c->who);
  sink_writes(ev_writer_sink(c->w), "250 paused\n");
  return 1;
}

static int c_shutdown(struct conn *c,
		      char attribute((unused)) **vec,
		      int attribute((unused)) nvec) {
  disorder_info("S%x shut down by %s", c->tag, c->who);
  sink_writes(ev_writer_sink(c->w), "250 shutting down\n");
  ev_writer_flush(c->w);
  quit(c->ev);
}

static int c_reconfigure(struct conn *c,
			 char attribute((unused)) **vec,
			 int attribute((unused)) nvec) {
  disorder_info("S%x reconfigure by %s", c->tag, c->who);
  if(reconfigure(c->ev, 1))
    sink_writes(ev_writer_sink(c->w), "550 error reading new config\n");
  else
    sink_writes(ev_writer_sink(c->w), "250 installed new config\n");
  return 1;				/* completed */
}

static void finished_rescan(void *ru) {
  struct conn *const c = ru;

  sink_writes(ev_writer_sink(c->w), "250 rescan completed\n");
  /* Turn this connection back on */
  ev_reader_enable(c->r);
}

static void start_fresh_rescan(void *ru) {
  struct conn *const c = ru;

  if(trackdb_rescan_underway()) {
    /* Some other waiter beat us to it.  However in this case we're happy to
     * piggyback; the requirement is that a new rescan be started, not that it
     * was _our_ rescan. */
    if(c->rescan_wait) {
      /* We block until the rescan completes */
      trackdb_add_rescanned(finished_rescan, c);
    } else {
      /* We report that the new rescan has started */
      sink_writes(ev_writer_sink(c->w), "250 rescan initiated\n");
      /* Turn this connection back on */
      ev_reader_enable(c->r);
    }
  } else {
    /* We are the first connection to get a callback so we must start a
     * rescan. */
    if(c->rescan_wait) {
      /* We want to block until the new rescan completes */
      trackdb_rescan(c->ev, 1/*check*/, finished_rescan, c);
    } else {
      /* We can report back immediately */
      trackdb_rescan(c->ev, 1/*check*/, 0, 0);
      sink_writes(ev_writer_sink(c->w), "250 rescan initiated\n");
      /* Turn this connection back on */
      ev_reader_enable(c->r);
    }
  }
}

static int c_rescan(struct conn *c,
		    char **vec,
		    int nvec) {
  int flag_wait = 0, flag_fresh = 0, n;

  /* Parse flags */
  for(n = 0; n < nvec; ++n) {
    if(!strcmp(vec[n], "wait"))
      flag_wait = 1;			/* wait for rescan to complete */
#if 0
    /* Currently disabled because untested (and hard to test). */
    else if(!strcmp(vec[n], "fresh"))
      flag_fresh = 1;			/* don't piggyback underway rescan */
#endif
    else {
      sink_writes(ev_writer_sink(c->w), "550 unknown flag\n");
      return 1;				/* completed */
    }
  }
  /* Report what was requested */
  disorder_info("S%x rescan by %s (%s %s)", c->tag, c->who,
		flag_wait ? "wait" : "",
		flag_fresh ? "fresh" : "");
  if(trackdb_rescan_underway()) {
    if(flag_fresh) {
      /* We want a fresh rescan but there is already one underway.  Arrange a
       * callback when it completes and then set off a new one. */
      c->rescan_wait = flag_wait;
      trackdb_add_rescanned(start_fresh_rescan, c);
      if(flag_wait)
	return 0;
      else {
	sink_writes(ev_writer_sink(c->w), "250 rescan queued\n");
	return 1;
      }
    } else {
      /* There's a rescan underway, and it's acceptable to piggyback on it */
      if(flag_wait) {
	/* We want to block until completion. */
	trackdb_add_rescanned(finished_rescan, c);
	return 0;
      } else {
	/* We don't want to block.  So we just report that things are in
	 * hand. */
	sink_writes(ev_writer_sink(c->w), "250 rescan already underway\n");
	return 1;
      }
    }
  } else {
    /* No rescan is underway.  fresh is therefore irrelevant. */
    if(flag_wait) {
      /* We want to block until completion */
      trackdb_rescan(c->ev, 1/*check*/, finished_rescan, c);
      return 0;
    } else {
      /* We don't want to block. */
      trackdb_rescan(c->ev, 1/*check*/, 0, 0);
      sink_writes(ev_writer_sink(c->w), "250 rescan initiated\n");
      return 1;				/* completed */
    }
  }
}

static int c_version(struct conn *c,
		     char attribute((unused)) **vec,
		     int attribute((unused)) nvec) {
  /* VERSION had better only use the basic character set */
  sink_printf(ev_writer_sink(c->w), "251 %s\n", disorder_short_version_string);
  return 1;			/* completed */
}

static int c_playing(struct conn *c,
		     char attribute((unused)) **vec,
		     int attribute((unused)) nvec) {
  if(playing) {
    queue_fix_sofar(playing);
    playing->expected = 0;
    sink_printf(ev_writer_sink(c->w), "252 %s\n", queue_marshall(playing));
  } else
    sink_printf(ev_writer_sink(c->w), "259 nothing playing\n");
  return 1;				/* completed */
}

static int c_playing_hls(struct conn *c,
		     char attribute((unused)) **vec,
		     int attribute((unused)) nvec) {
  char *url = 0, *encoded_track = 0;
  if (!config->hls_enable || !config->hls_baseurl) {
    sink_printf(ev_writer_sink(c->w), "550 HLS not enabled\n");
  }
  if(playing) {
    const char* bare_track = track_rootless(playing->track);
    if (bare_track == 0) {
      // can't join a scratch part-way through
      sink_printf(ev_writer_sink(c->w), "259 nothing playing\n");
    } else {
      encoded_track = urlencodestring(bare_track);
      byte_asprintf(&url, "%s%s", config->hls_baseurl, encoded_track);
      sink_printf(ev_writer_sink(c->w), "252 %lu %s\n", playing->played, url);
    }
  } else
    sink_printf(ev_writer_sink(c->w), "259 nothing playing\n");
  xfree(url);
  xfree(encoded_track);
  return 1;				/* completed */
}

static const char *connection_host(struct conn *c) {
  union {
    struct sockaddr sa;
    struct sockaddr_in in;
    struct sockaddr_in6 in6;
  } u;
  socklen_t l;
  int n;
  char host[1024];

  /* get connection data */
  l = sizeof u;
  if(getpeername(c->fd, &u.sa, &l) < 0) {
    disorder_error(errno, "S%x error calling getpeername", c->tag);
    return 0;
  }
  if(c->l->pf != PF_UNIX) {
    if((n = getnameinfo(&u.sa, l,
			host, sizeof host, 0, 0, NI_NUMERICHOST))) {
      disorder_error(0, "S%x error calling getnameinfo: %s",
		     c->tag, gai_strerror(n));
      return 0;
    }
    return xstrdup(host);
  } else
    return "local";
}

static int c_user(struct conn *c,
		  char **vec,
		  int attribute((unused)) nvec) {
  struct kvp *k;
  const char *res, *host, *password;
  rights_type rights;

  if(c->who) {
    sink_writes(ev_writer_sink(c->w), "530 already authenticated\n");
    return 1;
  }
  /* get connection data */
  if(!(host = connection_host(c))) {
    sink_writes(ev_writer_sink(c->w), "530 authentication failure\n");
    return 1;
  }
  /* find the user */
  k = trackdb_getuserinfo(vec[0]);
  /* reject nonexistent users */
  if(!k) {
    disorder_error(0, "S%x unknown user '%s' from %s", c->tag, vec[0], host);
    sink_writes(ev_writer_sink(c->w), "530 authentication failed\n");
    return 1;
  }
  /* reject unconfirmed users */
  if(kvp_get(k, "confirmation")) {
    disorder_error(0, "S%x unconfirmed user '%s' from %s",
		   c->tag, vec[0], host);
    sink_writes(ev_writer_sink(c->w), "530 authentication failed\n");
    return 1;
  }
  password = kvp_get(k, "password");
  if(!password) password = "";
  if(parse_rights(kvp_get(k, "rights"), &rights, 1)) {
    disorder_error(0, "error parsing rights for %s", vec[0]);
    sink_writes(ev_writer_sink(c->w), "530 authentication failed\n");
    return 1;
  }
  /* check whether the response is right */
  res = authhash(c->nonce, sizeof c->nonce, password,
		 config->authorization_algorithm);
  if(wideopen || c->l->privileged || (res && !strcmp(res, vec[1]))) {
    c->who = vec[0];
    c->rights = rights;
    /* currently we only bother logging remote connections */
    if(strcmp(host, "local"))
      disorder_info("S%x %s connected from %s", c->tag, vec[0], host);
    else
      c->rights |= RIGHT__LOCAL;
    sink_writes(ev_writer_sink(c->w), "230 OK\n");
    return 1;
  }
  /* oops, response was wrong */
  disorder_info("S%x authentication failure for %s from %s",
		c->tag, vec[0], host);
  sink_writes(ev_writer_sink(c->w), "530 authentication failed\n");
  return 1;
}

static int c_recent(struct conn *c,
		    char attribute((unused)) **vec,
		    int attribute((unused)) nvec) {
  const struct queue_entry *q;

  sink_writes(ev_writer_sink(c->w), "253 Tracks follow\n");
  for(q = phead.next; q != &phead; q = q->next)
    sink_printf(ev_writer_sink(c->w), " %s\n", queue_marshall(q));
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */
}

static int c_queue(struct conn *c,
		   char attribute((unused)) **vec,
		   int attribute((unused)) nvec) {
  struct queue_entry *q;
  time_t when = 0;
  const char *l;
  long length;

  sink_writes(ev_writer_sink(c->w), "253 Tracks follow\n");
  if(playing_is_enabled() && !paused) {
    if(playing) {
      queue_fix_sofar(playing);
      if((l = trackdb_get(playing->track, "_length"))
	 && (length = atol(l))) {
	xtime(&when);
	when += length - playing->sofar;
      }
    } else
      /* Nothing is playing but playing is enabled, so whatever is
       * first in the queue can be expected to start immediately. */
      xtime(&when);
  }
  for(q = qhead.next; q != &qhead; q = q->next) {
    /* fill in estimated start time */
    q->expected = when;
    sink_printf(ev_writer_sink(c->w), " %s\n", queue_marshall(q));
    /* update for next track */
    if(when) {
      if((l = trackdb_get(q->track, "_length"))
	 && (length = atol(l)))
	when += length;
      else
	when = 0;
    }
  }
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */
}

static int output_list(struct conn *c, char **vec) {
  while(*vec)
    sink_printf(ev_writer_sink(c->w), "%s\n", *vec++);
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;
}

static int files_dirs(struct conn *c,
		      char **vec,
		      int nvec,
		      enum trackdb_listable what) {
  const char *dir, *re;
  char errstr[RXCERR_LEN];
  size_t erroffset;
  regexp *rec;
  char **fvec, *key;
  
  switch(nvec) {
  case 0: dir = 0; re = 0; break;
  case 1: dir = vec[0]; re = 0; break;
  case 2: dir = vec[0]; re = vec[1]; break;
  default: abort();
  }
  /* A bit of a bodge to make sure the args don't trample on cache keys */
  if(dir && strchr(dir, '\n')) {
    sink_writes(ev_writer_sink(c->w), "550 invalid directory name\n");
    return 1;
  }
  if(re && strchr(re, '\n')) {
    sink_writes(ev_writer_sink(c->w), "550 invalid regexp\n");
    return 1;
  }
  /* We bother eliminating "" because the web interface is relatively
   * likely to send it */
  if(re && *re) {
    byte_xasprintf(&key, "%d\n%s\n%s", (int)what, dir ? dir : "", re);
    fvec = (char **)cache_get(&cache_files_type, key);
    if(fvec) {
      /* Got a cache hit, don't store the answer in the cache */
      key = 0;
      ++cache_files_hits;
      rec = 0;				/* quieten compiler */
    } else {
      /* Cache miss, we'll do the lookup and key != 0 so we'll store the answer
       * in the cache. */
      if(!(rec = regexp_compile(re, RXF_CASELESS,
				errstr, sizeof(errstr), &erroffset))) {
	sink_printf(ev_writer_sink(c->w), "550 Error compiling regexp: %s\n",
		    errstr);
	return 1;
      }
      /* It only counts as a miss if the regexp was valid. */
      ++cache_files_misses;
    }
  } else {
    /* No regexp, don't bother caching the result */
    rec = 0;
    key = 0;
    fvec = 0;
  }
  if(!fvec) {
    /* No cache hit (either because a miss, or because we did not look) so do
     * the lookup */
    if(dir && *dir)
      fvec = trackdb_list(dir, 0, what, rec);
    else
      fvec = trackdb_list(0, 0, what, rec);
  }
  if(key)
    /* Put the answer in the cache */
    cache_put(&cache_files_type, key, fvec);
  sink_writes(ev_writer_sink(c->w), "253 Listing follow\n");
  return output_list(c, fvec);
}

static int c_files(struct conn *c,
		  char **vec,
		  int nvec) {
  return files_dirs(c, vec, nvec, trackdb_files);
}

static int c_dirs(struct conn *c,
		  char **vec,
		  int nvec) {
  return files_dirs(c, vec, nvec, trackdb_directories);
}

static int c_allfiles(struct conn *c,
		      char **vec,
		      int nvec) {
  return files_dirs(c, vec, nvec, trackdb_directories|trackdb_files);
}

static int c_get(struct conn *c,
		 char **vec,
		 int attribute((unused)) nvec) {
  const char *v, *track;

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  if(vec[1][0] != '_' && (v = trackdb_get(track, vec[1])))
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(v));
  else
    sink_writes(ev_writer_sink(c->w), "555 not found\n");
  return 1;
}

static int c_length(struct conn *c,
		 char **vec,
		 int attribute((unused)) nvec) {
  const char *track, *v;

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  if((v = trackdb_get(track, "_length")))
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(v));
  else
    sink_writes(ev_writer_sink(c->w), "550 not found\n");
  return 1;
}

static int c_set(struct conn *c,
		 char **vec,
		 int attribute((unused)) nvec) {
  const char *track;

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  if(vec[1][0] != '_' && !trackdb_set(track, vec[1], vec[2]))
    sink_writes(ev_writer_sink(c->w), "250 OK\n");
  else
    sink_writes(ev_writer_sink(c->w), "550 not found\n");
  return 1;
}

static int c_prefs(struct conn *c,
		   char **vec,
		   int attribute((unused)) nvec) {
  struct kvp *k;
  const char *track;

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  k = trackdb_get_all(track);
  sink_writes(ev_writer_sink(c->w), "253 prefs follow\n");
  for(; k; k = k->next)
    if(k->name[0] != '_')		/* omit internal values */
      sink_printf(ev_writer_sink(c->w),
		  " %s %s\n", quoteutf8(k->name), quoteutf8(k->value));
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;
}

static int c_exists(struct conn *c,
		    char **vec,
		    int attribute((unused)) nvec) {
  /* trackdb_exists() does its own alias checking */
  sink_printf(ev_writer_sink(c->w), "252 %s\n", noyes[trackdb_exists(vec[0])]);
  return 1;
}

static void search_parse_error(const char *msg, void *u) {
  *(const char **)u = msg;
}

static int c_search(struct conn *c,
			  char **vec,
			  int attribute((unused)) nvec) {
  char **terms, **results;
  int nterms, nresults, n;
  const char *e = "unknown error";

  /* This is a bit of a bodge.  Initially it's there to make the eclient
   * interface a bit more convenient to add searching to, but it has the more
   * compelling advantage that if everything uses it, then interpretation of
   * user-supplied search strings will be the same everywhere. */
  if(!(terms = split(vec[0], &nterms, SPLIT_QUOTES, search_parse_error, &e))) {
    sink_printf(ev_writer_sink(c->w), "550 %s\n", e);
  } else {
    results = trackdb_search(terms, nterms, &nresults);
    sink_printf(ev_writer_sink(c->w), "253 %d matches\n", nresults);
    for(n = 0; n < nresults; ++n)
      sink_printf(ev_writer_sink(c->w), "%s\n", results[n]);
    sink_writes(ev_writer_sink(c->w), ".\n");
  }
  return 1;
}

static int c_random_enable(struct conn *c,
			   char attribute((unused)) **vec,
			   int attribute((unused)) nvec) {
  enable_random(c->who, c->ev);
  /* Enable implicitly unpauses if there is nothing playing */
  if(paused && !playing) resume_playing(c->who);
  sink_writes(ev_writer_sink(c->w), "250 OK\n");
  return 1;			/* completed */
}

static int c_random_disable(struct conn *c,
			    char attribute((unused)) **vec,
			    int attribute((unused)) nvec) {
  disable_random(c->who, c->ev);
  sink_writes(ev_writer_sink(c->w), "250 OK\n");
  return 1;			/* completed */
}

static int c_random_enabled(struct conn *c,
			    char attribute((unused)) **vec,
			    int attribute((unused)) nvec) {
  sink_printf(ev_writer_sink(c->w), "252 %s\n", noyes[random_is_enabled()]);
  return 1;			/* completed */
}

static void got_stats(char *stats, void *u) {
  struct conn *const c = u;

  sink_printf(ev_writer_sink(c->w), "253 stats\n%s\n.\n", stats);
  /* Now we can start processing commands again */
  ev_reader_enable(c->r);
}

static int c_stats(struct conn *c,
		   char attribute((unused)) **vec,
		   int attribute((unused)) nvec) {
  trackdb_stats_subprocess(c->ev, got_stats, c);
  return 0;				/* not yet complete */
}

static int c_volume(struct conn *c,
		    char **vec,
		    int nvec) {
  int l, r, set;
  char lb[32], rb[32];
  rights_type rights;

  switch(nvec) {
  case 0:
    set = 0;
    break;
  case 1:
    l = r = atoi(vec[0]);
    set = 1;
    break;
  case 2:
    l = atoi(vec[0]);
    r = atoi(vec[1]);
    set = 1;
    break;
  default:
    abort();
  }
  rights = set ? RIGHT_VOLUME : RIGHT_READ;
  if(!(c->rights & rights)) {
    disorder_error(0, "%s attempted to set volume but lacks required rights",
		   c->who);
    sink_writes(ev_writer_sink(c->w), "510 Prohibited\n");
    return 1;
  }
  if(!api || !api->set_volume) {
    sink_writes(ev_writer_sink(c->w), "550 error accessing mixer\n");
    return 1;
  }
  (set ? api->set_volume : api->get_volume)(&l, &r);
  sink_printf(ev_writer_sink(c->w), "252 %d %d\n", l, r);
  if(l != volume_left || r != volume_right) {
    volume_left = l;
    volume_right = r;
    snprintf(lb, sizeof lb, "%d", l);
    snprintf(rb, sizeof rb, "%d", r);
    eventlog("volume", lb, rb, (char *)0);
  }
  return 1;
}

/** @brief Called when data arrives on a log connection
 *
 * We just discard all such data.  The client may occasionally send data as a
 * keepalive.
 */
static int logging_reader_callback(ev_source attribute((unused)) *ev,
				   ev_reader *reader,
				   void attribute((unused)) *ptr,
				   size_t bytes,
				   int attribute((unused)) eof,
				   void attribute((unused)) *u) {
  struct conn *c = u;

  ev_reader_consume(reader, bytes);
  if(eof) {
    /* Oops, that's all for now */
    D(("logging reader eof"));
    if(c->w) {
      D(("close writer"));
      ev_writer_close(c->w);
      c->w = 0;
    }
    c->r = 0;
    remove_connection(c);
  }
  return 0;
}

static void logclient(const char *msg, void *user) {
  struct conn *c = user;

  if(!c->w || !c->r) {
    /* This connection has gone up in smoke for some reason */
    eventlog_remove(c->lo);
    c->lo = 0;
    return;
  }
  /* user_* messages are restricted */
  if(!strncmp(msg, "user_", 5)) {
    /* They are only sent to admin users */
    if(!(c->rights & RIGHT_ADMIN))
      return;
    /* They are not sent over TCP connections unless remote user-management is
     * enabled */
    if(!config->remote_userman && !(c->rights & RIGHT__LOCAL))
      return;
  }
  sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" %s\n",
	      (uintmax_t)xtime(0), msg);
}

static int c_log(struct conn *c,
		 char attribute((unused)) **vec,
		 int attribute((unused)) nvec) {
  time_t now;

  sink_writes(ev_writer_sink(c->w), "254 OK\n");
  /* pump out initial state */
  xtime(&now);
  sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" state %s\n",
	      (uintmax_t)now, 
	      playing_is_enabled() ? "enable_play" : "disable_play");
  sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" state %s\n",
	      (uintmax_t)now, 
	      random_is_enabled() ? "enable_random" : "disable_random");
  sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" state %s\n",
	      (uintmax_t)now, 
	      paused ? "pause" : "resume");
  if(playing) {
    sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" state playing\n",
		  (uintmax_t)now);
    if (config->hls_enable) {
      char *url = 0, *starttime = 0, *encoded_track = 0;
      const char *bare_track = track_rootless(playing->track);
      if (bare_track != 0)
      {
        encoded_track = urlencodestring(bare_track);
        byte_asprintf(&url, "%s%s", config->hls_baseurl, encoded_track);
        byte_asprintf(&starttime, "%lu", playing->played);
        sink_printf(ev_writer_sink(c->w), "%" PRIxMAX " hls_playout %s %s\n",
          (uintmax_t)now, starttime, url);
      } // else do nothing; scratches are too ephemeral to worry about here
      xfree(url);
      xfree(starttime);
      xfree(encoded_track);
    }
  }
  /* Initial volume */
  sink_printf(ev_writer_sink(c->w), "%"PRIxMAX" volume %d %d\n",
	      (uintmax_t)now, volume_left, volume_right);

  c->lo = xmalloc(sizeof *c->lo);
  c->lo->fn = logclient;
  c->lo->user = c;
  eventlog_add(c->lo);
  c->reader = logging_reader_callback;
  return 0;
}

/** @brief Test whether a move is allowed
 * @param c Connection
 * @param qs List of IDs on queue
 * @param nqs Number of IDs
 * @return 0 if move is prohibited, non-0 if it is allowed
 */
static int has_move_rights(struct conn *c, struct queue_entry **qs, int nqs) {
  for(; nqs > 0; ++qs, --nqs) {
    struct queue_entry *const q = *qs;

    if(!right_movable(c->rights, c->who, q))
      return 0;
  }
  return 1;
}

static int c_move(struct conn *c,
		  char **vec,
		  int attribute((unused)) nvec) {
  struct queue_entry *q;
  int n;

  if(!(q = queue_find(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 no such track on the queue\n");
    return 1;
  }
  if(!has_move_rights(c, &q, 1)) {
    disorder_error(0, "%s attempted move but lacks required rights", c->who);
    sink_writes(ev_writer_sink(c->w),
		"510 Not authorized to move that track\n");
    return 1;
  }
  n = queue_move(q, atoi(vec[1]), c->who);
  sink_printf(ev_writer_sink(c->w), "252 %d\n", n);
  /* If we've moved to the head of the queue then prepare the track. */
  if(q == qhead.next)
    prepare(c->ev, q);
  return 1;
}

static int c_moveafter(struct conn *c,
		       char **vec,
		       int attribute((unused)) nvec) {
  struct queue_entry *q, **qs;
  int n;

  if(vec[0][0]) {
    if(!(q = queue_find(vec[0]))) {
      sink_writes(ev_writer_sink(c->w), "550 no such track on the queue\n");
      return 1;
    }
  } else
    q = 0;
  ++vec;
  --nvec;
  qs = xcalloc(nvec, sizeof *qs);
  for(n = 0; n < nvec; ++n)
    if(!(qs[n] = queue_find(vec[n]))) {
      sink_writes(ev_writer_sink(c->w), "550 no such track on the queue\n");
      return 1;
    }
  if(!has_move_rights(c, qs, nvec)) {
    disorder_error(0, "%s attempted moveafter but lacks required rights",
		   c->who);
    sink_writes(ev_writer_sink(c->w),
		"510 Not authorized to move those tracks\n");
    return 1;
  }
  queue_moveafter(q, nvec, qs, c->who);
  sink_printf(ev_writer_sink(c->w), "250 Moved tracks\n");
  /* If we've moved to the head of the queue then prepare the track. */
  if(q == qhead.next)
    prepare(c->ev, q);
  return 1;
}

static int c_part(struct conn *c,
		  char **vec,
		  int attribute((unused)) nvec) {
  const char *track;

  int do_transform = 0;
  if (nvec > 3)
      do_transform = atoi(vec[3]);

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  if(do_transform) {
    const char *type = "dir";
    if (!strcmp(vec[2], "title"))
      type = "track";
    sink_printf(ev_writer_sink(c->w), "252 %s\n",
        quoteutf8(trackname_transform(type, trackdb_getpart(track, vec[1], vec[2]), vec[1])));
  } else {
    sink_printf(ev_writer_sink(c->w), "252 %s\n",
	    quoteutf8(trackdb_getpart(track, vec[1], vec[2])));
  }
  return 1;
}

static int c_resolve(struct conn *c,
		     char **vec,
		     int attribute((unused)) nvec) {
  const char *track;

  if(!(track = trackdb_resolve(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 cannot resolve track\n");
    return 1;
  }
  sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(track));
  return 1;
}

static int list_response(struct conn *c,
                         const char *reply,
                         char **list) {
  sink_printf(ev_writer_sink(c->w), "253 %s\n", reply);
  while(*list) {
    sink_printf(ev_writer_sink(c->w), "%s%s\n",
		**list == '.' ? "." : "", *list);
    ++list;
  }
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */
}

static int c_tags(struct conn *c,
		  char attribute((unused)) **vec,
		  int attribute((unused)) nvec) {
  return list_response(c, "Tag list follows", trackdb_alltags());
}

static int c_set_global(struct conn *c,
			char **vec,
			int attribute((unused)) nvec) {
  if(vec[0][0] == '_') {
    sink_writes(ev_writer_sink(c->w), "550 cannot set internal global preferences\n");
    return 1;
  }
  /* We special-case the 'magic' preferences here. */
  if(!strcmp(vec[0], "playing")) {
    (flag_enabled(vec[1]) ? enable_playing : disable_playing)(c->who, c->ev);
    sink_printf(ev_writer_sink(c->w), "250 OK\n");
  } else if(!strcmp(vec[0], "random-play")) {
    (flag_enabled(vec[1]) ? enable_random : disable_random)(c->who, c->ev);
    sink_printf(ev_writer_sink(c->w), "250 OK\n");
  } else {
    if(!trackdb_set_global(vec[0], vec[1], c->who))
      sink_printf(ev_writer_sink(c->w), "250 OK\n");
    else
      sink_writes(ev_writer_sink(c->w), "550 not found\n");
  }
  return 1;
}

static int c_get_global(struct conn *c,
			char **vec,
			int attribute((unused)) nvec) {
  const char *s = trackdb_get_global(vec[0]);

  if(s)
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(s));
  else
    sink_writes(ev_writer_sink(c->w), "555 not found\n");
  return 1;
}

static int c_nop(struct conn *c,
		 char attribute((unused)) **vec,
		 int attribute((unused)) nvec) {
  sink_printf(ev_writer_sink(c->w), "250 Quack\n");
  return 1;
}

static int c_new(struct conn *c,
		 char **vec,
		 int nvec) {
  int max;
  char **tracks;

  if(nvec > 0)
    max = atoi(vec[0]);
  else
    max = INT_MAX;
  if(max <= 0 || max > config->new_max)
    max = config->new_max;
  tracks = trackdb_new(0, max);
  sink_printf(ev_writer_sink(c->w), "253 New track list follows\n");
  while(*tracks) {
    sink_printf(ev_writer_sink(c->w), "%s%s\n",
		**tracks == '.' ? "." : "", *tracks);
    ++tracks;
  }
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */

}

static int c_rtp_address(struct conn *c,
			 char attribute((unused)) **vec,
			 int attribute((unused)) nvec) {
  if(api == &uaudio_rtp) {
    char **addr;

    if(!strcmp(config->rtp_mode, "request"))
      sink_printf(ev_writer_sink(c->w), "252 - -\n");
    else {
      netaddress_format(&config->broadcast, NULL, &addr);
      sink_printf(ev_writer_sink(c->w), "252 %s %s\n",
                  quoteutf8(addr[1]),
                  quoteutf8(addr[2]));
    }
  } else
    sink_writes(ev_writer_sink(c->w), "550 No RTP\n");
  return 1;
}

static int c_rtp_cancel(struct conn *c,
                        char attribute((unused)) **vec,
                        int attribute((unused)) nvec) {
  if(!c->rtp_requested) {
    sink_writes(ev_writer_sink(c->w), "550 No active RTP stream\n");
    return 1;
  }
  rtp_request_cancel(&c->rtp_destination);
  c->rtp_requested = 0;
  sink_writes(ev_writer_sink(c->w), "250 Cancelled RTP stream\n");
  return 1;
}

static int c_rtp_request(struct conn *c,
                         char **vec,
                         int attribute((unused)) nvec) {
  static const struct addrinfo hints = {
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_DGRAM,
    .ai_protocol = IPPROTO_UDP,
    .ai_flags = AI_NUMERICHOST|AI_NUMERICSERV,
  };
  struct addrinfo *res;
  int rc = getaddrinfo(vec[0], vec[1], &hints, &res);
  if(rc) {
    disorder_error(0, "%s port %s: %s",
                   vec[0], vec[1], gai_strerror(rc));
    sink_writes(ev_writer_sink(c->w), "550 Invalid address\n");
    return 1;
  }
  disorder_info("%s requested RTP stream to %s %s", c->who, vec[0], vec[1]);
  /* TODO might be useful to tighten this up to restrict clients to targetting
   * themselves only */
  if(c->rtp_requested) {
    rtp_request_cancel(&c->rtp_destination);
    c->rtp_requested = 0;
  }
  memcpy(&c->rtp_destination, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  rtp_request(&c->rtp_destination);
  c->rtp_requested = 1;
  sink_writes(ev_writer_sink(c->w), "250 Initiated RTP stream\n");
  // TODO teardown on connection close
  return 1;
}

static int c_cookie(struct conn *c,
		    char **vec,
		    int attribute((unused)) nvec) {
  const char *host;
  char *user;
  rights_type rights;

  /* Can't log in twice on the same connection */
  if(c->who) {
    sink_writes(ev_writer_sink(c->w), "530 already authenticated\n");
    return 1;
  }
  /* Get some kind of peer identifcation */
  if(!(host = connection_host(c))) {
    sink_writes(ev_writer_sink(c->w), "530 authentication failure\n");
    return 1;
  }
  /* Check the cookie */
  user = verify_cookie(vec[0], &rights);
  if(!user) {
    sink_writes(ev_writer_sink(c->w), "530 authentication failure\n");
    return 1;
  }
  /* Log in */
  c->who = user;
  c->cookie = vec[0];
  c->rights = rights;
  if(strcmp(host, "local"))
    disorder_info("S%x %s connected with cookie from %s", c->tag, user, host);
  else
    c->rights |= RIGHT__LOCAL;
  /* Response contains username so client knows who they are acting as */
  sink_printf(ev_writer_sink(c->w), "232 %s\n", quoteutf8(user));
  return 1;
}

static int c_make_cookie(struct conn *c,
			 char attribute((unused)) **vec,
			 int attribute((unused)) nvec) {
  const char *cookie = make_cookie(c->who);

  if(cookie)
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(cookie));
  else
    sink_writes(ev_writer_sink(c->w), "550 Cannot create cookie\n");
  return 1;
}

static int c_revoke(struct conn *c,
		    char attribute((unused)) **vec,
		    int attribute((unused)) nvec) {
  if(c->cookie) {
    revoke_cookie(c->cookie);
    sink_writes(ev_writer_sink(c->w), "250 OK\n");
  } else
    sink_writes(ev_writer_sink(c->w), "510 Did not log in with cookie\n");
  return 1;
}

static int c_adduser(struct conn *c,
		     char **vec,
		     int nvec) {
  const char *rights;

  if(!config->remote_userman && !(c->rights & RIGHT__LOCAL)) {
    disorder_error(0, "S%x: remote adduser", c->tag);
    sink_writes(ev_writer_sink(c->w), "510 Remote user management is disabled\n");
    return 1;
  }
  if(nvec > 2) {
    rights = vec[2];
    if(parse_rights(vec[2], 0, 1)) {
      sink_writes(ev_writer_sink(c->w), "550 Invalid rights list\n");
      return -1;
    }
  } else
    rights = config->default_rights;
  if(trackdb_adduser(vec[0], vec[1], rights,
		     0/*email*/, 0/*confirmation*/))
    sink_writes(ev_writer_sink(c->w), "550 Cannot create user\n");
  else
    sink_writes(ev_writer_sink(c->w), "250 User created\n");
  return 1;
}

static int c_deluser(struct conn *c,
		     char **vec,
		     int attribute((unused)) nvec) {
  struct conn *d;

  if(!config->remote_userman && !(c->rights & RIGHT__LOCAL)) {
    disorder_error(0, "S%x: remote deluser", c->tag);
    sink_writes(ev_writer_sink(c->w), "510 Remote user management is disabled\n");
    return 1;
  }
  if(trackdb_deluser(vec[0])) {
    sink_writes(ev_writer_sink(c->w), "550 Cannot delete user\n");
    return 1;
  }
  /* Zap connections belonging to deleted user */
  for(d = connections; d; d = d->next)
    if(!strcmp(d->who, vec[0]))
      d->rights = 0;
  sink_writes(ev_writer_sink(c->w), "250 User deleted\n");
  return 1;
}

static int c_edituser(struct conn *c,
		      char **vec,
		      int attribute((unused)) nvec) {
  struct conn *d;

  if(!config->remote_userman && !(c->rights & RIGHT__LOCAL)) {
    disorder_error(0, "S%x: remote edituser", c->tag);
    sink_writes(ev_writer_sink(c->w), "510 Remote user management is disabled\n");
    return 1;
  }
  /* RIGHT_ADMIN can do anything; otherwise you can only set your own email
   * address and password. */
  if((c->rights & RIGHT_ADMIN)
     || (!strcmp(c->who, vec[0])
	 && (!strcmp(vec[1], "email")
	     || !strcmp(vec[1], "password")))) {
    if(trackdb_edituserinfo(vec[0], vec[1], vec[2])) {
      sink_writes(ev_writer_sink(c->w), "550 Failed to change setting\n");
      return 1;
    }
    if(!strcmp(vec[1], "password")) {
      /* Zap all connections for this user after a password change */
      for(d = connections; d; d = d->next)
	if(!strcmp(d->who, vec[0]))
	  d->rights = 0;
    } else if(!strcmp(vec[1], "rights")) {
      /* Update rights for this user */
      rights_type r;

      if(!parse_rights(vec[2], &r, 1)) {
        const char *new_rights = rights_string(r);
	for(d = connections; d; d = d->next) {
	  if(!strcmp(d->who, vec[0])) {
            /* Update rights */
	    d->rights = r;
            /* Notify any log connections */
            if(d->lo)
              sink_printf(ev_writer_sink(d->w),
                          "%"PRIxMAX" rights_changed %s\n",
                          (uintmax_t)xtime(0),
                          quoteutf8(new_rights));
          }
        }
      }
    }
    sink_writes(ev_writer_sink(c->w), "250 OK\n");
  } else {
    disorder_error(0, "%s attempted edituser but lacks required rights",
		   c->who);
    sink_writes(ev_writer_sink(c->w), "510 Restricted to administrators\n");
  }
  return 1;
}

static int c_userinfo(struct conn *c,
		      char attribute((unused)) **vec,
		      int attribute((unused)) nvec) {
  struct kvp *k;
  const char *value;

  /* We allow remote querying of rights so that clients can figure out what
   * they're allowed to do */
  if(!config->remote_userman
     && !(c->rights & RIGHT__LOCAL)
     && strcmp(vec[1], "rights")) {
    disorder_error(0, "S%x: remote userinfo %s %s", c->tag, vec[0], vec[1]);
    sink_writes(ev_writer_sink(c->w), "510 Remote user management is disabled\n");
    return 1;
  }
  /* RIGHT_ADMIN allows anything; otherwise you can only get your own email
   * address and rights list. */
  if((c->rights & RIGHT_ADMIN)
     || (!strcmp(c->who, vec[0])
	 && (!strcmp(vec[1], "email")
	     || !strcmp(vec[1], "rights")))) {
    if((k = trackdb_getuserinfo(vec[0])))
      if((value = kvp_get(k, vec[1])))
	sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(value));
      else
	sink_writes(ev_writer_sink(c->w), "555 Not set\n");
    else
      sink_writes(ev_writer_sink(c->w), "550 No such user\n");
  } else {
    disorder_error(0, "%s attempted userinfo but lacks required rights",
		   c->who);
    sink_writes(ev_writer_sink(c->w), "510 Restricted to administrators\n");
  }
  return 1;
}

static int c_users(struct conn *c,
		   char attribute((unused)) **vec,
		   int attribute((unused)) nvec) {
  return list_response(c, "User list follows", trackdb_listusers());
}

static int c_register(struct conn *c,
		      char **vec,
		      int attribute((unused)) nvec) {
  char *cs;
  uint32_t nonce[CONFIRM_SIZE];
  char nonce_str[(32 * CONFIRM_SIZE) / 5 + 1];

  /* The confirmation string is username/base62(nonce).  The confirmation
   * process will pick the username back out to identify them but the _whole_
   * string is used as the confirmation string.  Base 62 means we used only
   * letters and digits, minimizing the chance of the URL being mispasted. */
  gcry_randomize(nonce, sizeof nonce, GCRY_STRONG_RANDOM);
  if(basen(nonce, CONFIRM_SIZE, nonce_str, sizeof nonce_str, 62)) {
    disorder_error(0, "buffer too small encoding confirmation string");
    sink_writes(ev_writer_sink(c->w), "550 Cannot create user\n");
  }
  byte_xasprintf(&cs, "%s/%s", vec[0], nonce_str);
  if(trackdb_adduser(vec[0], vec[1], config->default_rights, vec[2], cs))
    sink_writes(ev_writer_sink(c->w), "550 Cannot create user\n");
  else
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(cs));
  return 1;
}

static int c_confirm(struct conn *c,
		     char **vec,
		     int attribute((unused)) nvec) {
  char *user, *sep;
  rights_type rights;
  const char *host;

  /* Get some kind of peer identifcation */
  if(!(host = connection_host(c))) {
    sink_writes(ev_writer_sink(c->w), "530 Authentication failure\n");
    return 1;
  }
  /* Picking the LAST / means we don't (here) rule out slashes in usernames. */
  if(!(sep = strrchr(vec[0], '/'))) {
    sink_writes(ev_writer_sink(c->w), "550 Malformed confirmation string\n");
    return 1;
  }
  user = xstrndup(vec[0], sep - vec[0]);
  if(trackdb_confirm(user, vec[0], &rights))
    sink_writes(ev_writer_sink(c->w), "510 Incorrect confirmation string\n");
  else {
    c->who = user;
    c->cookie = 0;
    c->rights = rights;
    if(strcmp(host, "local"))
      disorder_info("S%x %s confirmed from %s", c->tag, user, host);
    else
      c->rights |= RIGHT__LOCAL;
    /* Response contains username so client knows who they are acting as */
    sink_printf(ev_writer_sink(c->w), "232 %s\n", quoteutf8(user));
  }
  return 1;
}

static int sent_reminder(ev_source attribute((unused)) *ev,
			 pid_t attribute((unused)) pid,
			 int status,
			 const struct rusage attribute((unused)) *rusage,
			 void *u) {
  struct conn *const c = u;

  /* Tell the client what went down */ 
  if(!status) {
    sink_writes(ev_writer_sink(c->w), "250 OK\n");
  } else {
    disorder_error(0, "reminder subprocess %s", wstat(status));
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
  }
  /* Re-enable this connection */
  ev_reader_enable(c->r);
  return 0;
}

static int c_reminder(struct conn *c,
		      char **vec,
		      int attribute((unused)) nvec) {
  struct kvp *k;
  const char *password, *email, *text, *encoding, *charset, *content_type;
  const time_t *last;
  time_t now;
  pid_t pid;
  
  static hash *last_reminder;

  if(!config->mail_sender) {
    disorder_error(0, "cannot send password reminders because mail_sender not set");
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  if(!(k = trackdb_getuserinfo(vec[0]))) {
    disorder_error(0, "reminder for user '%s' who does not exist", vec[0]);
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  if(!(email = kvp_get(k, "email"))
     || !email_valid(email)) {
    disorder_error(0, "user '%s' has no valid email address", vec[0]);
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  if(!(password = kvp_get(k, "password"))
     || !*password) {
    disorder_error(0, "user '%s' has no password", vec[0]);
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  /* Rate-limit reminders.  This hash is bounded in size by the number of
   * users.  If this is actually a problem for anyone then we can periodically
   * clean it. */
  if(!last_reminder)
    last_reminder = hash_new(sizeof (time_t));
  last = hash_find(last_reminder, vec[0]);
  xtime(&now);
  if(last && now < *last + config->reminder_interval) {
    disorder_error(0, "sent a password reminder to '%s' too recently", vec[0]);
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  /* Send the reminder */
  /* TODO this should be templatized and to some extent merged with
   * the code in act_register() */
  byte_xasprintf((char **)&text,
"Someone requested that you be sent a reminder of your DisOrder password.\n"
"Your password is:\n"
"\n"
"  %s\n", password);
  if(!(text = mime_encode_text(text, &charset, &encoding)))
    disorder_fatal(0, "cannot encode email");
  byte_xasprintf((char **)&content_type, "text/plain;charset=%s",
		 quote822(charset, 0));
  pid = sendmail_subprocess("", config->mail_sender, email,
			    "DisOrder password reminder",
			    encoding, content_type, text);
  if(pid < 0) {
    sink_writes(ev_writer_sink(c->w), "550 Cannot send a reminder email\n");
    return 1;
  }
  hash_add(last_reminder, vec[0], &now, HASH_INSERT_OR_REPLACE);
  disorder_info("sending a passsword reminder to user '%s'", vec[0]);
  /* We can only continue when the subprocess finishes */
  ev_child(c->ev, pid, 0, sent_reminder, c);
  return 0;
}

static int c_schedule_list(struct conn *c,
			   char attribute((unused)) **vec,
			   int attribute((unused)) nvec) {
  char **ids = schedule_list(0);
  sink_writes(ev_writer_sink(c->w), "253 ID list follows\n");
  while(*ids)
    sink_printf(ev_writer_sink(c->w), "%s\n", *ids++);
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */
}

static int c_schedule_get(struct conn *c,
			  char **vec,
			  int attribute((unused)) nvec) {
  struct kvp *actiondata = schedule_get(vec[0]), *k;

  if(!actiondata) {
    sink_writes(ev_writer_sink(c->w), "555 No such event\n");
    return 1;				/* completed */
  }
  /* Scheduled events are public information.  Anyone with RIGHT_READ can see
   * them. */
  sink_writes(ev_writer_sink(c->w), "253 Event information follows\n");
  for(k = actiondata; k; k = k->next)
    sink_printf(ev_writer_sink(c->w), " %s %s\n",
		quoteutf8(k->name),  quoteutf8(k->value));
  sink_writes(ev_writer_sink(c->w), ".\n");
  return 1;				/* completed */
}

static int c_schedule_del(struct conn *c,
			  char **vec,
			  int attribute((unused)) nvec) {
  struct kvp *actiondata = schedule_get(vec[0]);

  if(!actiondata) {
    sink_writes(ev_writer_sink(c->w), "555 No such event\n");
    return 1;				/* completed */
  }
  /* If you have admin rights you can delete anything.  If you don't then you
   * can only delete your own scheduled events. */
  if(!(c->rights & RIGHT_ADMIN)) {
    const char *who = kvp_get(actiondata, "who");

    if(!who || !c->who || strcmp(who, c->who)) {
      sink_writes(ev_writer_sink(c->w), "510 Not authorized\n");
      return 1;				/* completed */
    }
  }
  if(schedule_del(vec[0]))
    sink_writes(ev_writer_sink(c->w), "550 Could not delete scheduled event\n");
  else
    sink_writes(ev_writer_sink(c->w), "250 Deleted\n");
  return 1;				/* completed */
}

static int c_schedule_add(struct conn *c,
			  char **vec,
			  int nvec) {
  struct kvp *actiondata = 0;
  const char *id;

  /* Standard fields */
  kvp_set(&actiondata, "who", c->who);
  kvp_set(&actiondata, "when", vec[0]);
  kvp_set(&actiondata, "priority", vec[1]);
  kvp_set(&actiondata, "action", vec[2]);
  /* Action-dependent fields */
  if(!strcmp(vec[2], "play")) {
    if(nvec != 4) {
      sink_writes(ev_writer_sink(c->w), "550 Wrong number of arguments\n");
      return 1;
    }
    if(!trackdb_exists(vec[3])) {
      sink_writes(ev_writer_sink(c->w), "550 Track is not in database\n");
      return 1;
    }
    kvp_set(&actiondata, "track", vec[3]);
  } else if(!strcmp(vec[2], "set-global")) {
    if(nvec < 4 || nvec > 5) {
      sink_writes(ev_writer_sink(c->w), "550 Wrong number of arguments\n");
      return 1;
    }
    kvp_set(&actiondata, "key", vec[3]);
    if(nvec > 4)
      kvp_set(&actiondata, "value", vec[4]);
  } else {
    sink_writes(ev_writer_sink(c->w), "550 Unknown action\n");
    return 1;
  }
  /* schedule_add() checks user rights */
  id = schedule_add(c->ev, actiondata);
  if(!id)
    sink_writes(ev_writer_sink(c->w), "550 Cannot add scheduled event\n");
  else
    sink_printf(ev_writer_sink(c->w), "252 %s\n", id);
  return 1;
}

static int c_adopt(struct conn *c,
		   char **vec,
		   int attribute((unused)) nvec) {
  struct queue_entry *q;

  if(!c->who) {
    sink_writes(ev_writer_sink(c->w), "550 no identity\n");
    return 1;
  }
  if(!(q = queue_find(vec[0]))) {
    sink_writes(ev_writer_sink(c->w), "550 no such track on the queue\n");
    return 1;
  }
  if(q->origin != origin_random) {
    sink_writes(ev_writer_sink(c->w), "550 not a random track\n");
    return 1;
  }
  q->origin = origin_adopted;
  q->submitter = xstrdup(c->who);
  eventlog("adopted", q->id, q->submitter, (char *)0);
  queue_write();
  sink_writes(ev_writer_sink(c->w), "250 OK\n");
  return 1;
}

static int playlist_response(struct conn *c,
                             int err) {
  switch(err) {
  case 0:
    assert(!"cannot cope with success");
  case EACCES:
    sink_writes(ev_writer_sink(c->w), "510 Access denied\n");
    break;
  case EINVAL:
    sink_writes(ev_writer_sink(c->w), "550 Invalid playlist name\n");
    break;
  case ENOENT:
    sink_writes(ev_writer_sink(c->w), "555 No such playlist\n");
    break;
  default:
    sink_writes(ev_writer_sink(c->w), "550 Error accessing playlist\n");
    break;
  }
  return 1;
}

static int c_playlist_get(struct conn *c,
			  char **vec,
			  int attribute((unused)) nvec) {
  char **tracks;
  int err;

  if(!(err = trackdb_playlist_get(vec[0], c->who, &tracks, 0, 0)))
    return list_response(c, "Playlist contents follows", tracks);
  else
    return playlist_response(c, err);
}

static int c_playlist_set(struct conn *c,
			  char **vec,
			  int attribute((unused)) nvec) {
  return fetch_body(c, c_playlist_set_body, vec[0]);
}

static int c_playlist_set_body(struct conn *c,
                               char **body,
                               int nbody,
                               void *u) {
  const char *playlist = u;
  int err;

  if(!c->locked_playlist
     || strcmp(playlist, c->locked_playlist)) {
    sink_writes(ev_writer_sink(c->w), "550 Playlist is not locked\n");
    return 1;
  }
  if(!(err = trackdb_playlist_set(playlist, c->who,
                                  body, nbody, 0))) {
    sink_printf(ev_writer_sink(c->w), "250 OK\n");
    return 1;
  } else
    return playlist_response(c, err);
}

static int c_playlist_get_share(struct conn *c,
                                char **vec,
                                int attribute((unused)) nvec) {
  char *share;
  int err;

  if(!(err = trackdb_playlist_get(vec[0], c->who, 0, 0, &share))) {
    sink_printf(ev_writer_sink(c->w), "252 %s\n", quoteutf8(share));
    return 1;
  } else
    return playlist_response(c, err);
}

static int c_playlist_set_share(struct conn *c,
                                char **vec,
                                int attribute((unused)) nvec) {
  int err;

  if(!(err = trackdb_playlist_set(vec[0], c->who, 0, 0, vec[1]))) {
    sink_printf(ev_writer_sink(c->w), "250 OK\n");
    return 1;
  } else
    return playlist_response(c, err);
}

static int c_playlists(struct conn *c,
                       char attribute((unused)) **vec,
                       int attribute((unused)) nvec) {
  char **p;

  trackdb_playlist_list(c->who, &p, 0);
  return list_response(c, "List of playlists follows", p);
}

static int c_playlist_delete(struct conn *c,
                             char **vec,
                             int attribute((unused)) nvec) {
  int err;
  
  if(!(err = trackdb_playlist_delete(vec[0], c->who))) {
    sink_writes(ev_writer_sink(c->w), "250 OK\n");
    return 1;
  } else
    return playlist_response(c, err);
}

static int c_playlist_lock(struct conn *c,
                           char **vec,
                           int attribute((unused)) nvec) {
  int err;
  struct conn *cc;

  /* Check we're allowed to modify this playlist */
  if((err = trackdb_playlist_set(vec[0], c->who, 0, 0, 0)))
    return playlist_response(c, err);
  /* If we hold a lock don't allow a new one */
  if(c->locked_playlist) {
    sink_writes(ev_writer_sink(c->w), "550 Already holding a lock\n");
    return 1;
  }
  /* See if some other connection locks the same playlist */
  for(cc = connections; cc; cc = cc->next)
    if(cc->locked_playlist && !strcmp(cc->locked_playlist, vec[0]))
      break;
  if(cc) {
    /* TODO: implement config->playlist_lock_timeout */
    sink_writes(ev_writer_sink(c->w), "550 Already locked\n");
    return 1;
  }
  c->locked_playlist = xstrdup(vec[0]);
  time(&c->locked_when);
  sink_writes(ev_writer_sink(c->w), "250 Acquired lock\n");
  return 1;
}

static int c_playlist_unlock(struct conn *c,
                             char attribute((unused)) **vec,
                             int attribute((unused)) nvec) {
  if(!c->locked_playlist) {
    sink_writes(ev_writer_sink(c->w), "550 Not holding a lock\n");
    return 1;
  }
  c->locked_playlist = 0;
  sink_writes(ev_writer_sink(c->w), "250 Released lock\n");
  return 1;
}

/** @brief Server's definition of a command */
static const struct server_command {
  /** @brief Command name */
  const char *name;

  /** @brief Minimum number of arguments */
  int minargs;

  /** @brief Maximum number of arguments */
  int maxargs;

  /** @brief Function to process command */
  int (*fn)(struct conn *, char **, int);

  /** @brief Rights required to execute command
   *
   * 0 means that the command can be issued without logging in.  If multiple
   * bits are listed here any of those rights will do.
   */
  rights_type rights;
} commands[] = {
  { "adduser",        2, 3,       c_adduser,        RIGHT_ADMIN },
  { "adopt",          1, 1,       c_adopt,          RIGHT_PLAY },
  { "allfiles",       0, 2,       c_allfiles,       RIGHT_READ },
  { "confirm",        1, 1,       c_confirm,        0 },
  { "cookie",         1, 1,       c_cookie,         0 },
  { "deluser",        1, 1,       c_deluser,        RIGHT_ADMIN },
  { "dirs",           0, 2,       c_dirs,           RIGHT_READ },
  { "disable",        0, 1,       c_disable,        RIGHT_GLOBAL_PREFS },
  { "edituser",       3, 3,       c_edituser,       RIGHT_ADMIN|RIGHT_USERINFO },
  { "enable",         0, 0,       c_enable,         RIGHT_GLOBAL_PREFS },
  { "enabled",        0, 0,       c_enabled,        RIGHT_READ },
  { "exists",         1, 1,       c_exists,         RIGHT_READ },
  { "files",          0, 2,       c_files,          RIGHT_READ },
  { "get",            2, 2,       c_get,            RIGHT_READ },
  { "get-global",     1, 1,       c_get_global,     RIGHT_READ },
  { "length",         1, 1,       c_length,         RIGHT_READ },
  { "log",            0, 0,       c_log,            RIGHT_READ },
  { "make-cookie",    0, 0,       c_make_cookie,    RIGHT_READ },
  { "move",           2, 2,       c_move,           RIGHT_MOVE__MASK },
  { "moveafter",      1, INT_MAX, c_moveafter,      RIGHT_MOVE__MASK },
  { "new",            0, 1,       c_new,            RIGHT_READ },
  { "nop",            0, 0,       c_nop,            0 },
  { "part",           3, 4,       c_part,           RIGHT_READ },
  { "pause",          0, 0,       c_pause,          RIGHT_PAUSE },
  { "play",           1, 1,       c_play,           RIGHT_PLAY },
  { "playafter",      2, INT_MAX, c_playafter,      RIGHT_PLAY },
  { "playing",        0, 0,       c_playing,        RIGHT_READ },
  { "playing-hls",    0, 0,       c_playing_hls,    RIGHT_READ },
  { "playlist-delete",    1, 1,   c_playlist_delete,    RIGHT_PLAY },
  { "playlist-get",       1, 1,   c_playlist_get,       RIGHT_READ },
  { "playlist-get-share", 1, 1,   c_playlist_get_share, RIGHT_READ },
  { "playlist-lock",      1, 1,   c_playlist_lock,      RIGHT_PLAY },
  { "playlist-set",       1, 1,   c_playlist_set,       RIGHT_PLAY },
  { "playlist-set-share", 2, 2,   c_playlist_set_share, RIGHT_PLAY },
  { "playlist-unlock",    0, 0,   c_playlist_unlock,    RIGHT_PLAY },
  { "playlists",          0, 0,   c_playlists,          RIGHT_READ },
  { "prefs",          1, 1,       c_prefs,          RIGHT_READ },
  { "queue",          0, 0,       c_queue,          RIGHT_READ },
  { "random-disable", 0, 0,       c_random_disable, RIGHT_GLOBAL_PREFS },
  { "random-enable",  0, 0,       c_random_enable,  RIGHT_GLOBAL_PREFS },
  { "random-enabled", 0, 0,       c_random_enabled, RIGHT_READ },
  { "recent",         0, 0,       c_recent,         RIGHT_READ },
  { "reconfigure",    0, 0,       c_reconfigure,    RIGHT_ADMIN },
  { "register",       3, 3,       c_register,       RIGHT_REGISTER },
  { "reminder",       1, 1,       c_reminder,       RIGHT__LOCAL },
  { "remove",         1, 1,       c_remove,         RIGHT_REMOVE__MASK },
  { "rescan",         0, INT_MAX, c_rescan,         RIGHT_RESCAN },
  { "resolve",        1, 1,       c_resolve,        RIGHT_READ },
  { "resume",         0, 0,       c_resume,         RIGHT_PAUSE },
  { "revoke",         0, 0,       c_revoke,         RIGHT_READ },
  { "rtp-address",    0, 0,       c_rtp_address,    0 },
  { "rtp-cancel",     0, 0,       c_rtp_cancel,     0 },
  { "rtp-request",    2, 2,       c_rtp_request,    RIGHT_READ },
  { "schedule-add",   3, INT_MAX, c_schedule_add,   RIGHT_READ },
  { "schedule-del",   1, 1,       c_schedule_del,   RIGHT_READ },
  { "schedule-get",   1, 1,       c_schedule_get,   RIGHT_READ },
  { "schedule-list",  0, 0,       c_schedule_list,  RIGHT_READ },
  { "scratch",        0, 1,       c_scratch,        RIGHT_SCRATCH__MASK },
  { "search",         1, 1,       c_search,         RIGHT_READ },
  { "set",            3, 3,       c_set,            RIGHT_PREFS, },
  { "set-global",     2, 2,       c_set_global,     RIGHT_GLOBAL_PREFS },
  { "shutdown",       0, 0,       c_shutdown,       RIGHT_ADMIN },
  { "stats",          0, 0,       c_stats,          RIGHT_READ },
  { "tags",           0, 0,       c_tags,           RIGHT_READ },
  { "unset",          2, 2,       c_set,            RIGHT_PREFS },
  { "unset-global",   1, 1,       c_set_global,     RIGHT_GLOBAL_PREFS },
  { "user",           2, 2,       c_user,           0 },
  { "userinfo",       2, 2,       c_userinfo,       RIGHT_READ },
  { "users",          0, 0,       c_users,          RIGHT_READ },
  { "version",        0, 0,       c_version,        RIGHT_READ },
  { "volume",         0, 2,       c_volume,         RIGHT_READ|RIGHT_VOLUME }
};

/** @brief Fetch a command body
 * @param c Connection
 * @param body_callback Called with body
 * @param u Passed to body_callback
 * @return 1
 */
static int fetch_body(struct conn *c,
                      body_callback_type body_callback,
                      void *u) {
  assert(c->line_reader == command);
  c->line_reader = body_line;
  c->body_callback = body_callback;
  c->body_u = u;
  vector_init(c->body);
  return 1;
}

/** @brief @ref line_reader_type callback for command body lines
 * @param c Connection
 * @param line Line
 * @return 1 if complete, 0 if incomplete
 *
 * Called from reader_callback().
 */
static int body_line(struct conn *c,
                     char *line) {
  if(*line == '.') {
    ++line;
    if(!*line) {
      /* That's the lot */
      c->line_reader = command;
      vector_terminate(c->body);
      return c->body_callback(c, c->body->vec, c->body->nvec, c->body_u);
    }
  }
  vector_append(c->body, xstrdup(line));
  return 1;                             /* completed */
}

static void command_error(const char *msg, void *u) {
  struct conn *c = u;

  sink_printf(ev_writer_sink(c->w), "500 parse error: %s\n", msg);
}

/** @brief @ref line_reader_type callback for commands
 * @param c Connection
 * @param line Line
 * @return 1 if complete, 0 if incomplete
 *
 * Called from reader_callback().
 */
static int command(struct conn *c, char *line) {
  char **vec;
  int nvec, n;

  D(("server command %s", line));
  /* We force everything into NFC as early as possible */
  if(!(line = utf8_compose_canon(line, strlen(line), 0))) {
    sink_writes(ev_writer_sink(c->w), "500 cannot normalize command\n");
    return 1;
  }
  if(!(vec = split(line, &nvec, SPLIT_QUOTES, command_error, c))) {
    sink_writes(ev_writer_sink(c->w), "500 cannot parse command\n");
    return 1;
  }
  if(nvec == 0) {
    sink_writes(ev_writer_sink(c->w), "500 do what?\n");
    return 1;
  }
  if((n = TABLE_FIND(commands, name, vec[0])) < 0)
    sink_writes(ev_writer_sink(c->w), "500 unknown command\n");
  else {
    if(commands[n].rights
       && !(c->rights & commands[n].rights)) {
      disorder_error(0, "%s attempted %s but lacks required rights",
		     c->who ? c->who : "NULL",
	    commands[n].name);
      sink_writes(ev_writer_sink(c->w), "510 Prohibited\n");
      return 1;
    }
    ++vec;
    --nvec;
    if(nvec < commands[n].minargs) {
      sink_writes(ev_writer_sink(c->w), "500 missing argument(s)\n");
      return 1;
    }
    if(nvec > commands[n].maxargs) {
      sink_writes(ev_writer_sink(c->w), "500 too many arguments\n");
      return 1;
    }
    return commands[n].fn(c, vec, nvec);
  }
  return 1;			/* completed */
}

/* redirect to the right reader callback for our current state */
static int redirect_reader_callback(ev_source *ev,
				    ev_reader *reader,
				    void *ptr,
				    size_t bytes,
				    int eof,
				    void *u) {
  struct conn *c = u;

  return c->reader(ev, reader, ptr, bytes, eof, u);
}

/* the main command reader */
static int reader_callback(ev_source attribute((unused)) *ev,
			   ev_reader *reader,
			   void *ptr,
			   size_t bytes,
			   int eof,
			   void *u) {
  struct conn *c = u;
  char *eol;
  int complete;

  D(("server reader_callback"));
  while((eol = memchr(ptr, '\n', bytes))) {
    *eol++ = 0;
    ev_reader_consume(reader, eol - (char *)ptr);
    complete = c->line_reader(c, ptr);  /* usually command() */
    bytes -= (eol - (char *)ptr);
    ptr = eol;
    if(!complete) {
      /* the command had better have set a new reader callback */
      if(bytes || eof)
	/* there are further bytes to read, or we are at eof; arrange for the
	 * command's reader callback to handle them */
	return ev_reader_incomplete(reader);
      /* nothing's going on right now */
      return 0;
    }
    /* command completed, we can go around and handle the next one */
  }
  if(eof) {
    if(bytes)
      disorder_error(0, "S%x unterminated line", c->tag);
    D(("normal reader close"));
    c->r = 0;
    if(c->w) {
      D(("close associated writer"));
      ev_writer_close(c->w);
      c->w = 0;
    }
    remove_connection(c);
  }
  return 0;
}

static int listen_callback(ev_source *ev,
			   int fd,
			   const struct sockaddr attribute((unused)) *remote,
			   socklen_t attribute((unused)) rlen,
			   void *u) {
  const struct listener *l = u;
  struct conn *c = xmalloc(sizeof *c);
  static unsigned tags;

  D(("server listen_callback fd %d (%s)", fd, l->name));
  nonblock(fd);
  cloexec(fd);
  c->next = connections;
  c->tag = tags++;
  c->ev = ev;
  c->w = ev_writer_new(ev, fd, writer_error, c,
		       "client writer");
  if(!c->w) {
    disorder_error(0, "ev_writer_new for file inbound connection (fd=%d) failed",
          fd);
    close(fd);
    return 0;
  }
  c->r = ev_reader_new(ev, fd, redirect_reader_callback, reader_error, c,
		       "client reader");
  if(!c->r)
    /* Main reason for failure is the FD is too big and that will already have
     * been handled */
    disorder_fatal(0,
		   "ev_reader_new for file inbound connection (fd=%d) failed",
		   fd);
  ev_tie(c->r, c->w);
  c->fd = fd;
  c->reader = reader_callback;
  c->l = l;
  c->rights = 0;
  c->line_reader = command;
  connections = c;
  gcry_randomize(c->nonce, sizeof c->nonce, GCRY_STRONG_RANDOM);
  sink_printf(ev_writer_sink(c->w), "231 %d %s %s\n",
	      2,
	      config->authorization_algorithm,
	      hex(c->nonce, sizeof c->nonce));
  return 0;
}

int server_start(ev_source *ev, int pf,
		 size_t socklen, const struct sockaddr *sa,
		 const char *name,
		 int privileged) {
  int fd;
  struct listener *l = xmalloc(sizeof *l);
  static const int one = 1;

  D(("server_init socket %s privileged=%d", name, privileged));
  /* Sanity check */
  if(privileged && pf != AF_UNIX)
    disorder_fatal(0, "cannot create a privileged listener on a non-local port");
  fd = xsocket(pf, SOCK_STREAM, 0);
  xsetsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if(bind(fd, sa, socklen) < 0) {
    disorder_error(errno, "error binding to %s", name);
    return -1;
  }
  xlisten(fd, 128);
  nonblock(fd);
  cloexec(fd);
  l->name = name;
  l->pf = pf;
  l->privileged = privileged;
  if(ev_listen(ev, fd, listen_callback, l, "server listener"))
    exit(EXIT_FAILURE);
  disorder_info("listening on %s", name);
  return fd;
}

int server_stop(ev_source *ev, int fd) {
  xclose(fd);
  return ev_listen_cancel(ev, fd);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
End:
*/
