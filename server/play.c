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
/** @file server/play.c
 * @brief Playing tracks
 *
 * This file is rather badly organized.  Sorry.  It's better than it was...
 */

#include "disorder-server.h"

#define SPEAKER "disorder-speaker"

/** @brief The current playing track or NULL */
struct queue_entry *playing;

/** @brief Set when paused */
int paused;

static void finished(ev_source *ev);
static int start_child(struct queue_entry *q, 
                       const struct pbgc_params *params,
                       void attribute((unused)) *bgdata);
static int prepare_child(struct queue_entry *q, 
                         const struct pbgc_params *params,
                         void attribute((unused)) *bgdata);
static void ensure_next_scratch(ev_source *ev);

/** @brief File descriptor of our end of the socket to the speaker */
static int speaker_fd = -1;

/** @brief Set when shutting down */
static int shutting_down;

static void hls_playing(struct queue_entry *q);

/* Speaker ------------------------------------------------------------------ */

/** @brief Called when speaker process terminates
 *
 * Currently kills of DisOrder completely.  A future version could terminate
 * the speaker when nothing was going on, or recover from failures, though any
 * tracks with decoders already started would need to have them restarted.
 */
static int speaker_terminated(ev_source attribute((unused)) *ev,
			      pid_t attribute((unused)) pid,
			      int attribute((unused)) status,
			      const struct rusage attribute((unused)) *rusage,
			      void attribute((unused)) *u) {
  disorder_fatal(0, "speaker subprocess %s", wstat(status));
}

/** @brief Called when we get a message from the speaker process */
static int speaker_readable(ev_source *ev, int fd,
			    void attribute((unused)) *u) {
  struct speaker_message sm;
  int ret = speaker_recv(fd, &sm);
  
  if(ret < 0) return 0;			/* EAGAIN */
  if(!ret) {				/* EOF */
    ev_fd_cancel(ev, ev_read, fd);
    return 0;
  }
  switch(sm.type) {
  case SM_PAUSED:
    /* track ID is paused, DATA seconds played */
    D(("SM_PAUSED %s %ld", sm.u.id, sm.data));
    playing->sofar = sm.data;
    break;
  case SM_FINISHED:			/* scratched the playing track */
  case SM_STILLBORN:			/* scratched too early */
  case SM_UNKNOWN:			/* scratched WAY too early */
    if(playing && !strcmp(sm.u.id, playing->id)) {
      if((playing->state == playing_unplayed
          || playing->state == playing_started)
         && sm.type == SM_FINISHED)
        playing->state = playing_ok;
      finished(ev);
    }
    break;
  case SM_PLAYING:
    /* track ID is playing, DATA seconds played */
    D(("SM_PLAYING %s %ld", sm.u.id, sm.data));
    playing->sofar = sm.data;
    break;
  case SM_ARRIVED: {
    /* track ID is now prepared */
    struct queue_entry *q;
    for(q = qhead.next; q != &qhead && strcmp(q->id, sm.u.id); q = q->next)
      ;
    if(q && q->preparing) {
      q->preparing = 0;
      q->prepared = 1;
      /* We might be waiting to play the now-prepared track */
      play(ev);
    }
    break;
  }
  default:
    disorder_error(0, "unknown speaker message type %d", sm.type);
  }
  return 0;
}

/** @brief Initialize the speaker process */
void speaker_setup(ev_source *ev) {
  int sp[2];
  pid_t pid;
  struct speaker_message sm;

  if(socketpair(PF_UNIX, SOCK_DGRAM, 0, sp) < 0)
    disorder_fatal(errno, "error calling socketpair");
  if(!(pid = xfork())) {
    exitfn = _exit;
    ev_signal_atfork(ev);
    xdup2(sp[0], 0);
    xdup2(sp[0], 1);
    xclose(sp[0]);
    xclose(sp[1]);
    signal(SIGPIPE, SIG_DFL);
#if 0
    execlp("valgrind", "valgrind", SPEAKER, "--config", configfile,
	   debugging ? "--debug" : "--no-debug",
	   log_default == &log_syslog ? "--syslog" : "--no-syslog",
	   (char *)0);
#else
    execlp(SPEAKER, SPEAKER, "--config", configfile,
	   debugging ? "--debug" : "--no-debug",
	   log_default == &log_syslog ? "--syslog" : "--no-syslog",
	   (char *)0);
#endif
    disorder_fatal(errno, "error invoking %s", SPEAKER);
  }
  ev_child(ev, pid, 0, speaker_terminated, 0);
  speaker_fd = sp[1];
  xclose(sp[0]);
  cloexec(speaker_fd);
  /* Wait for the speaker to be ready */
  speaker_recv(speaker_fd, &sm);
  nonblock(speaker_fd);
  if(ev_fd(ev, ev_read, speaker_fd, speaker_readable, 0, "speaker read") < 0)
    disorder_fatal(0, "error registering speaker socket fd");
}

/** @brief Tell the speaker to reload its configuration */
void speaker_reload(void) {
  struct speaker_message sm;

  memset(&sm, 0, sizeof sm);
  sm.type = SM_RELOAD;
  speaker_send(speaker_fd, &sm);
}

/* Track termination -------------------------------------------------------- */

/** @brief Called when the currently playing track finishes playing
 * @param ev Event loop or NULL
 *
 * There are three places this is called from:
 * 
 * 1) speaker_readable(), when the speaker tells us the playing track finished.
 * (Technically the speaker lies a little to arrange for gapless play.)
 *
 * 2) player_finished(), when the player for a non-raw track (i.e. one that
 * does not use the speaker) finishes.
 *
 * 3) quitting(), after signalling the decoder or player but possible before it
 * has actually terminated.  In this case @p ev is NULL, inhibiting any further
 * attempt to play anything.
 */
static void finished(ev_source *ev) {
  D(("finished playing=%p", (void *)playing));
  if(!playing)
    return;
  if(playing->state != playing_scratched)
    notify_not_scratched(playing->track, playing->submitter);
  switch(playing->state) {
  case playing_ok:
    eventlog("completed", playing->track, (char *)0);
    break;
  case playing_scratched:
    eventlog("scratched", playing->track, playing->scratched, (char *)0);
    break;
  case playing_failed:
    eventlog("failed", playing->track, wstat(playing->wstat), (char *)0);
    break;
  default:
    break;
  }
  queue_played(playing);
  recent_write();
  playing = 0;
  /* Try to play something else */
  if(ev)
    play(ev);
}

/** @brief Called when a player or decoder process terminates
 *
 * This is called when a decoder process terminates (which might actually be
 * some time before the speaker reports it as finished) or when a non-raw
 * (i.e. non-speaker) player terminates.  In the latter case it's imaginable
 * that the OS has buffered the last few samples.
 *
 * NB.  The finished track might NOT be in the queue (yet) - it might be a
 * pre-chosen scratch.
 */
static int player_finished(ev_source *ev,
			   pid_t pid,
			   int status,
			   const struct rusage attribute((unused)) *rusage,
			   void *u) {
  struct queue_entry *q = u;

  D(("player_finished pid=%lu status=%#x",
     (unsigned long)pid, (unsigned)status));
  /* Record that this PID is dead.  If we killed the track we might know this
   * already, but also it might have exited or crashed.  Either way we don't
   * want to end up signalling it. */
  q->pid = -1;
  switch(q->state) {
  case playing_unplayed:
  case playing_random:
    /* If this was a pre-prepared track then either it failed or we
     * deliberately stopped it: it might have been removed from the queue, or
     * moved down the queue, or the speaker might be on a break.  So we leave
     * it state alone for future use.
     */
    break;
  default:
    /* We actually started playing this track. */
    if(status) {
      /* Don't override 'scratched' with 'failed'. */
      if(q->state != playing_scratched)
	q->state = playing_failed;
    } else 
      q->state = playing_ok;
    break;
  }
  /* Report the status unless we killed it */
  if(status) {
    if(!(q->killed && WIFSIGNALED(status) && WTERMSIG(status) == q->killed))
      disorder_error(0, "player for %s %s", q->track, wstat(status));
  }
  /* Clean up any prefork calls */
  if(q->type & DISORDER_PLAYER_PREFORK)
    play_cleanup(q->pl, q->data);
  q->wstat = status;
  /* If this actually was the current track, and does not use the speaker
   * process, then it must have finished.  For raw-output players we will get a
   * separate notification from the speaker process. */
  if(q == playing
     && (q->type & DISORDER_PLAYER_TYPEMASK) != DISORDER_PLAYER_RAW)
    finished(ev);
  return 0;
}

/* Track initiation --------------------------------------------------------- */

/** @brief Find the player for @p q */
static const struct stringlist *find_player(const struct queue_entry *q) {
  int n;
  
  for(n = 0; n < config->player.n; ++n)
    if(fnmatch(config->player.s[n].s[0], q->track, 0) == 0)
      break;
  if(n >= config->player.n)
    return NULL;
  else
    return &config->player.s[n];
}

/** @brief Start to play @p q
 * @param ev Event loop
 * @param q Track to play/prepare
 * @return @ref START_OK, @ref START_HARDFAIL or @ref START_SOFTFAIL
 *
 * This makes @p actually start playing.  It calls prepare() if necessary and
 * either sends an @ref SM_PLAY command or invokes the player itself in a
 * subprocess.
 *
 * It's up to the caller to set @ref playing and @c playing->state (this might
 * be changed in the future).
 */
static int start(ev_source *ev,
		 struct queue_entry *q) {
  const struct stringlist *player;
  int rc;

  D(("start %s", q->id));
  /* Find the player plugin. */
  if(!(player = find_player(q)))
    return START_HARDFAIL;              /* No player */
  if(!(q->pl = open_plugin(player->s[1], 0)))
    return START_HARDFAIL;
  q->type = play_get_type(q->pl);
  /* Special handling for raw-format players */
  if((q->type & DISORDER_PLAYER_TYPEMASK) == DISORDER_PLAYER_RAW) {
    /* Make sure that the track is prepared */
    if((rc = prepare(ev, q)))
      return rc;
    /* Now we're sure it's prepared, start it playing */
    /* TODO actually it might not be fully prepared yet - it's all happening in
     * a subprocess.  See speaker.c for further discussion.  */
    struct speaker_message sm[1];
    memset(sm, 0, sizeof sm);
    strcpy(sm->u.id, q->id);
    sm->type = SM_PLAY;
    speaker_send(speaker_fd, sm);
    D(("sent SM_PLAY for %s", sm->u.id));
    /* Our caller will set playing and playing->state = playing_started */
    return START_OK;
  } else {
    rc = play_background(ev, player, q, start_child, NULL);
    if(rc == START_OK)
      ev_child(ev, q->pid, 0, player_finished, q);
      /* Our caller will set playing and playing->state = playing_started */
    return rc;
  }
}

/** @brief Child-process half of start()
 * @return Process exit code
 *
 * Called in subprocess to execute non-raw-format players (via plugin).
 */
static int start_child(struct queue_entry *q, 
                       const struct pbgc_params *params,
                       void attribute((unused)) *bgdata) {
  /* Play the track */
  play_track(q->pl,
             params->argv, params->argc,
             params->rawpath,
             q->track);
  return 0;
}

/** @brief Prepare a track for later play
 * @return @ref START_OK, @ref START_HARDFAIL or @ref START_SOFTFAIL
 *
 * This can be called either when we want to play the track or slightly before
 * so that some samples are decoded and available in a buffer.
 *
 * Only applies to raw-format (i.e. speaker-using) players; everything else
 * gets @c START_OK.
 */
int prepare(ev_source *ev,
	    struct queue_entry *q) {
  const struct stringlist *player;

  /* If there's a decoder (or player!) going we do nothing */
  if(q->pid >= 0)
    return START_OK;
  /* If the track is already prepared, do nothing */
  if(q->prepared || q->preparing)
    return START_OK;
  /* Find the player plugin */
  if(!(player = find_player(q))) 
    return START_HARDFAIL;              /* No player */
  q->pl = open_plugin(player->s[1], 0);
  q->type = play_get_type(q->pl);
  if((q->type & DISORDER_PLAYER_TYPEMASK) != DISORDER_PLAYER_RAW)
    return START_OK;                    /* Not a raw player */
  int rc = play_background(ev, player, q, prepare_child, NULL);
  if(rc == START_OK) {
    ev_child(ev, q->pid, 0, player_finished, q);
    q->preparing = 1;
    /* Actually the track is still "in flight" */
    rc = START_SOFTFAIL;
  }
  return rc;
}

/** @brief Child-process half of prepare()
 * @return Process exit code
 *
 * Called in subprocess to execute the decoder for a raw-format player.
 *
 * @todo We currently run the normalizer from here in a double-fork.  This is
 * unsatisfactory for many reasons: we can't prevent it outliving the main
 * server and we don't adequately report its exit status.
 */
static int prepare_child(struct queue_entry *q, 
                         const struct pbgc_params *params,
                         void attribute((unused)) *bgdata) {
  /* np will be the pipe to disorder-normalize */
  int np[2];
  if(socketpair(PF_UNIX, SOCK_STREAM, 0, np) < 0)
    disorder_fatal(errno, "error calling socketpair");
  /* Beware of the Leopard!  On OS X 10.5.x, the order of the shutdown
   * calls here DOES MATTER.  If you do the SHUT_WR first then the SHUT_RD
   * fails with "Socket is not connected".  I think this is a bug but
   * provided implementors either don't care about the order or all agree
   * about the order, choosing the reliable order is an adequate
   * workaround.  */
  xshutdown(np[1], SHUT_RD);	/* decoder writes to np[1] */
  xshutdown(np[0], SHUT_WR);	/* normalize reads from np[0] */
  blocking(np[0]);
  blocking(np[1]);
  /* Start disorder-normalize.  We double-fork so that nothing has to wait
   * for disorder-normalize. */
  pid_t npid;
  if(!(npid = xfork())) {
    /* Grandchild of disorderd */
    if(!xfork()) {
      /* Great-grandchild of disorderd */
      /* Connect to the speaker process */
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof addr);
      addr.sun_family = AF_UNIX;
      snprintf(addr.sun_path, sizeof addr.sun_path,
               "%s/private/speaker", config->home);
      int sfd = xsocket(PF_UNIX, SOCK_STREAM, 0);
      if(connect(sfd, (const struct sockaddr *)&addr, sizeof addr) < 0)
        disorder_fatal(errno, "connecting to %s", addr.sun_path);
      /* Send the ID, with a NATIVE-ENDIAN 32 bit length */
      uint32_t l = strlen(q->id);
      if(write(sfd, &l, sizeof l) < 0
         || write(sfd, q->id, l) < 0)
        disorder_fatal(errno, "writing to %s", addr.sun_path);
      /* Await the ack */
      if (read(sfd, &l, 1) < 0) 
        disorder_fatal(errno, "reading ack from %s", addr.sun_path);
      /* Plumbing */
      xdup2(np[0], 0);
      xdup2(sfd, 1);
      xclose(np[0]);
      xclose(np[1]);
      xclose(sfd);
      /* TODO stderr shouldn't be redirected for disorder-normalize
       * (but it should be for play_track() */
      execlp("disorder-normalize", "disorder-normalize",
             log_default == &log_syslog ? "--syslog" : "--no-syslog",
             "--config", configfile,
             (char *)0);
      disorder_fatal(errno, "executing disorder-normalize");
      /* End of the great-grandchild of disorderd */
    }
    /* Back in the grandchild of disorderd */
    _exit(0);
    /* End of the grandchild of disorderd */
  }
  /* Back in the child of disorderd */
  /* Wait for the grandchild of disordered to finish */
  int n;
  while(waitpid(npid, &n, 0) < 0 && errno == EINTR)
    ;
  /* Pass the file descriptor to the driver in an environment
   * variable. */
  char buffer[64];
  snprintf(buffer, sizeof buffer, "DISORDER_RAW_FD=%d", np[1]);
  if(putenv(buffer) < 0)
    disorder_fatal(errno, "error calling putenv");
  /* Close all the FDs we don't need */
  xclose(np[0]);
  /* Start the decoder itself */
  play_track(q->pl,
             params->argv, params->argc,
             params->rawpath,
             q->track);
  return 0;
}

/** @brief Kill a player
 * @param q Queue entry corresponding to player
 */
static void kill_player(struct queue_entry *q) {
  if(q->pid >= 0)
    kill(-q->pid, config->signal);
  q->killed = config->signal;
}

/** @brief Abandon a queue entry
 *
 * Called from c_remove() (but NOT when scratching a track).  Only does
 * anything to raw-format tracks.  Terminates the background decoder and tells
 * the speaker process to cancel the track.
 */
void abandon(ev_source attribute((unused)) *ev,
	     struct queue_entry *q) {
  struct speaker_message sm;

  if(q->pid < 0)
    return;                             /* Not prepared. */
  if((q->type & DISORDER_PLAYER_TYPEMASK) != DISORDER_PLAYER_RAW)
    return;				/* Not a raw player. */
  /* Terminate the player. */
  kill_player(q);
  /* Cancel the track. */
  memset(&sm, 0, sizeof sm);
  sm.type = SM_CANCEL;
  strcpy(sm.u.id, q->id);
  speaker_send(speaker_fd, &sm);
}

/* Random tracks ------------------------------------------------------------ */

/** @brief Called with a new random track
 * @param ev Event loop
 * @param track Track name
 */
static void chosen_random_track(ev_source *ev,
				const char *track) {
  struct queue_entry *q;

  if(!track)
    return;
  /* Add the track to the queue */
  q = queue_add(track, 0, WHERE_END, NULL, origin_random);
  D(("picked %p (%s) at random", (void *)q, q->track));
  queue_write();
  /* Maybe a track can now be played */
  play(ev);
}

/** @brief Maybe add a randomly chosen track
 * @param ev Event loop
 *
 * Picking can take some time so the track will only be added after this
 * function has returned.
 */
void add_random_track(ev_source *ev) {
  struct queue_entry *q;
  long qlen = 0;

  /* If random play is not enabled then do nothing. */
  if(shutting_down || !random_is_enabled())
    return;
  /* Count how big the queue is */
  for(q = qhead.next; q != &qhead; q = q->next)
    ++qlen;
  /* If it's smaller than the desired size then add a track */
  if(qlen < config->queue_pad)
    trackdb_request_random(ev, chosen_random_track);
}

/* Track initiation (part 2) ------------------------------------------------ */

/** @brief Attempt to play something
 *
 * This is called from numerous locations - whenever it might conceivably have
 * become possible to play something.
 */
void play(ev_source *ev) {
  struct queue_entry *q;
  int random_enabled = random_is_enabled();

  D(("play playing=%p", (void *)playing));
  /* If we're shutting down, or there's something playing, or playing is not
   * enabled, give up now */
  if(shutting_down || playing || !playing_is_enabled()) return;
  /* See if there's anything to play */
  if(qhead.next == &qhead) {
    /* Queue is empty.  We could just wait around since there are periodic
     * attempts to add a random track anyway.  However they are rarer than
     * attempts to force a track so we initiate one now. */
    add_random_track(ev);
    /* chosen_random_track() will call play() when a new random track has been
     * added to the queue. */
    return;
  }
  /* There must be at least one track in the queue. */
  q = qhead.next;
  /* If random play is disabled but the track is a non-adopted random one
   * then don't play it.  play() will be called again when random play is
   * re-enabled. */
  if(!random_enabled && q->origin == origin_random)
    return;
  D(("taken %p (%s) from queue", (void *)q, q->track));
  /* Try to start playing. */
  switch(start(ev, q)) {
  case START_HARDFAIL:
    if(q == qhead.next) {
      queue_remove(q, 0);		/* Abandon this track. */
      queue_played(q);
      recent_write();
    }
    /* Oh well, try the next one */
    play(ev);
    break;
  case START_SOFTFAIL:
    /* We'll try the same track again shortly. */
    break;
  case START_OK:
    /* Remove from the queue */
    if(q == qhead.next) {
      queue_remove(q, 0);
      queue_write();
    }
    /* It's become the playing track */
    playing = q;
    xtime(&playing->played);
    playing->state = playing_started;
    notify_play(playing->track, playing->submitter);
    eventlog("playing", playing->track,
	     playing->submitter ? playing->submitter : (const char *)0,
	     (const char *)0);
    hls_playing(playing);
    /* Maybe add a random track. */
    add_random_track(ev);
    /* If there is another track in the queue prepare it now.  This could
     * potentially be a just-added random track. */
    if(qhead.next != &qhead)
      prepare(ev, qhead.next);
    /* Make sure there is a prepared scratch */
    ensure_next_scratch(ev);
    break;
  }
}

/* Miscelleneous ------------------------------------------------------------ */

int flag_enabled(const char *s) {
  return !s || !strcmp(s, "yes");
}

/** @brief Return true if play is enabled */
int playing_is_enabled(void) {
  return flag_enabled(trackdb_get_global("playing"));
}

/** @brief Enable play */
void enable_playing(const char *who, ev_source *ev) {
  trackdb_set_global("playing", "yes", who);
  /* Add a random track if necessary. */
  add_random_track(ev);
  play(ev);
}

/** @brief Disable play */
void disable_playing(const char *who, ev_source attribute((unused)) *ev) {
  trackdb_set_global("playing", "no", who);
}

/** @brief Return true if random play is enabled */
int random_is_enabled(void) {
  return flag_enabled(trackdb_get_global("random-play"));
}

/** @brief Enable random play */
void enable_random(const char *who, ev_source *ev) {
  trackdb_set_global("random-play", "yes", who);
  add_random_track(ev);
  play(ev);
}

/** @brief Disable random play */
void disable_random(const char *who, ev_source attribute((unused)) *ev) {
  trackdb_set_global("random-play", "no", who);
}

/* Scratching --------------------------------------------------------------- */

/** @brief Track to play next time something is scratched */
static struct queue_entry *next_scratch;

/** @brief Ensure there isa prepared scratch */
static void ensure_next_scratch(ev_source *ev) {
  if(next_scratch)                      /* There's one already */
    return;
  if(!config->scratch.n)                /* There are no scratches */
    return;
  int r = rand() * (double)config->scratch.n / (RAND_MAX + 1.0);
  next_scratch = queue_add(config->scratch.s[r], NULL,
                           WHERE_NOWHERE, NULL, origin_scratch);
  if(ev)
    prepare(ev, next_scratch);
}

/** @brief Scratch a track
 * @param who User responsible (or NULL)
 * @param id Track ID (or NULL for current)
 */
void scratch(const char *who, const char *id) {
  struct speaker_message sm;

  D(("scratch playing=%p state=%d id=%s playing->id=%s",
     (void *)playing,
     playing ? playing->state : 0,
     id ? id : "(none)",
     playing ? playing->id : "(none)"));
  /* There must be a playing track; it must be in a scratchable state; if a
   * specific ID was mentioned it must be that track. */
  if(playing
     && (playing->state == playing_started
	 || playing->state == playing_paused)
     && (!id
	 || !strcmp(id, playing->id))) {
    /* Update state (for the benefit of the 'recent' list) */
    playing->state = playing_scratched;
    playing->scratched = who ? xstrdup(who) : 0;
    /* Find the player and kill the whole process group */
    if(playing->pid >= 0)
      kill_player(playing);
    /* Tell the speaker, if we think it'll care */
    if((playing->type & DISORDER_PLAYER_TYPEMASK) == DISORDER_PLAYER_RAW) {
      memset(&sm, 0, sizeof sm);
      sm.type = SM_CANCEL;
      strcpy(sm.u.id, playing->id);
      speaker_send(speaker_fd, &sm);
      D(("sending SM_CANCEL for %s", playing->id));
    }
    /* If playing is enabled then add a scratch to the queue.  Having a scratch
     * appear in the queue when further play is disabled is weird and
     * contradicts implicit assumptions made elsewhere, so we try to avoid
     * it. */
    if(playing_is_enabled()) {
      /* Try to make sure there is a scratch */
      ensure_next_scratch(NULL);
      /* Insert it at the head of the queue */
      if(next_scratch){
        next_scratch->submitter = who;
        queue_insert_entry(&qhead, next_scratch);
        eventlog_raw("queue", queue_marshall(next_scratch), (const char *)0);
        next_scratch = NULL;
      }
    }
    notify_scratch(playing->track, playing->submitter, who,
		   xtime(0) - playing->played);
  }
}

/* Server termination ------------------------------------------------------- */

/** @brief Called from quit() to tear down everything belonging to this file */
void quitting(ev_source *ev) {
  struct queue_entry *q;

  /* Don't start anything new */
  shutting_down = 1;
  /* Shut down the current player */
  if(playing) {
    kill_player(playing);
    playing->state = playing_quitting;
    finished(0);
  }
  /* Zap any background decoders that are going */
  for(q = qhead.next; q != &qhead; q = q->next)
    kill_player(q);
  /* Don't need the speaker any more */
  ev_fd_cancel(ev, ev_read, speaker_fd);
  xclose(speaker_fd);
}

/* Pause and resume --------------------------------------------------------- */

/** @brief Pause the playing track */
int pause_playing(const char *who) {
  struct speaker_message sm;
  long played;
  
  /* Can't pause if already paused or if nothing playing. */
  if(!playing || paused) return 0;
  switch(playing->type & DISORDER_PLAYER_TYPEMASK) {
  case DISORDER_PLAYER_STANDALONE:
    if(!(playing->type & DISORDER_PLAYER_PAUSES)) {
    default:
      disorder_error(0,  "cannot pause because player is not powerful enough");
      return -1;
    }
    if(play_pause(playing->pl, &played, playing->data)) {
      disorder_error(0, "player indicates it cannot pause");
      return -1;
    }
    xtime(&playing->lastpaused);
    playing->uptopause = played;
    playing->lastresumed = 0;
    break;
  case DISORDER_PLAYER_RAW:
    memset(&sm, 0, sizeof sm);
    sm.type = SM_PAUSE;
    speaker_send(speaker_fd, &sm);
    break;
  }
  if(who)
    disorder_info("paused by %s", who);
  notify_pause(playing->track, who);
  paused = 1;
  if(playing->state == playing_started)
    playing->state = playing_paused;
  eventlog("state", "pause", (char *)0);
  return 0;
}

/** @brief Resume playing after a pause */
void resume_playing(const char *who) {
  struct speaker_message sm;

  if(!paused) return;
  paused = 0;
  if(!playing) return;
  switch(playing->type & DISORDER_PLAYER_TYPEMASK) {
  case DISORDER_PLAYER_STANDALONE:
    if(!(playing->type & DISORDER_PLAYER_PAUSES)) {
    default:
      /* Shouldn't happen */
      return;
    }
    play_resume(playing->pl, playing->data);
    xtime(&playing->lastresumed);
    break;
  case DISORDER_PLAYER_RAW:
    memset(&sm, 0, sizeof sm);
    sm.type = SM_RESUME;
    speaker_send(speaker_fd, &sm);
    break;
  }
  if(who) disorder_info("resumed by %s", who);
  notify_resume(playing->track, who);
  if(playing->state == playing_paused)
    playing->state = playing_started;
  eventlog("state", "resume", (char *)0);
}

/** @brief Request an RTP stream */
void rtp_request(const struct sockaddr_storage *sa) {
  struct speaker_message sm;
  memset(&sm, 0, sizeof sm);
  sm.type = SM_RTP_REQUEST;
  sm.u.address = *sa;
  speaker_send(speaker_fd, &sm);
}

/** @brief Cancel an RTP stream */
void rtp_request_cancel(const struct sockaddr_storage *sa) {
  struct speaker_message sm;
  memset(&sm, 0, sizeof sm);
  sm.type = SM_RTP_CANCEL;
  sm.u.address = *sa;
  speaker_send(speaker_fd, &sm);
}

/* HLS streaming support ---------------------------------------------------- */

/** @brief Searches for a collection with a URL mapping that matches a given track.
 * This is used for scratches.
 * @param track The track of interest
 * @param baseurl (Output) If a matching collection was found, this is the base
 * URL for that collection.
 * @return If a matching collection was found, the track name relative to the
 * collection base URL. (If no collection found, NULL.)
 */
static const char* hls_find_scratch(const char*track, const char**baseurl) {
  for (int i=0; i<config->hls_urlmap.n; i++) {
    const char *key = config->hls_urlmap.m[i].key;
    int len = strlen(key);
    if (0==strncmp(track, key, len)) {
      *baseurl = config->hls_urlmap.m[i].url;
      return &track[len];
    }
  }
  return 0;
}

static void hls_playing(struct queue_entry *q) {
  if (!config->hls_enable)
    return;
  const char *track_root = find_track_root(q->track);
  const char *baseurl = 0, *bare_track = 0;
  if (!bare_track) {
    bare_track = hls_find_scratch(q->track, &baseurl);
    if (!bare_track) {
      disorder_error(0, "could not find scratch mapping for %s", q->track);
      return;
    }
    // else continue on..
  } else {
    baseurl = urlmap_for(&config->hls_urlmap, track_root);
    if (!baseurl) {
      disorder_error(0, "No mapped URL for %s", track_root);
      return;
    }
    bare_track = track_rootless(q->track); // this is not alloc'd
  }

  char *url = 0, *sofar= 0, *encoded_track = 0;
  encoded_track = urlencodestring(bare_track); // This is malloc'd
  byte_xasprintf(&url, "%s%s", baseurl, encoded_track);
  if (url && *url) {
    byte_xasprintf(&sofar, "%lu", q->sofar);
    eventlog("hls_playout", sofar, url, (char*)0);
  }
  xfree(url);
  xfree(sofar);
  xfree(encoded_track);
}


/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
