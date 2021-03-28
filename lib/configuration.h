/*
 * This file is part of DisOrder.
 * Copyright (C) 2004-2011, 2013 Richard Kettlewell
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
/** @file lib/configuration.h
 * @brief Configuration file support
 */

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "speaker-protocol.h"
#include "regexp.h"
#include "rights.h"
#include "addr.h"

struct uaudio;

/* Configuration is kept in a @struct config@; the live configuration
 * is always pointed to by @config@.  Values in @config@ are UTF-8 encoded.
 */

/** @brief A list of strings */
struct stringlist {
  /** @brief Number of strings */
  int n;
  /** @brief Array of strings */
  char **s;
};

/** @brief A list of list of strings */
struct stringlistlist {
  /** @brief Number of string lists */
  int n;
  /** @brief Array of string lists */
  struct stringlist *s;
};

/** @brief A collection of tracks */
struct collection {
  /** @brief Module that supports this collection */
  char *module;
  /** @brief Filename encoding */
  char *encoding;
  /** @brief Root directory */
  char *root;
};

/** @brief A list of collections */
struct collectionlist {
  /** @brief Number of collections */
  int n;
  /** @brief Array of collections */
  struct collection *s;
};

/** @brief A track name part */
struct namepart {
  char *part;				/* part */
  regexp *re;				/* compiled regexp */
  char *res;                            /* regexp as a string */
  char *replace;			/* replacement string */
  char *context;			/* context glob */
  unsigned reflags;			/* regexp flags */
};

/** @brief A list of track name parts */
struct namepartlist {
  int n;
  struct namepart *s;
};

/** @brief A track name transform */
struct transform {
  char *type;				/* track or dir */
  char *context;			/* sort or choose */
  char *replace;			/* substitution string */
  regexp *re;				/* compiled re */
  unsigned flags;			/* regexp flags */
};

/** @brief A list of track name transforms */
struct transformlist {
  int n;
  struct transform *t;
};

/** @brief A mapping from collection base to URL base */
struct urlmap {
  char *key;
  char *url;
};
struct urlmap_list {
  int n;
  struct urlmap *m;
};

/** @brief System configuration */
struct config {
  /* server config */

  /** @brief Authorization algorithm */
  char *authorization_algorithm;
  
  /** @brief All players */
  struct stringlistlist player;

  /** @brief All tracklength plugins */
  struct stringlistlist tracklength;

  /** @brief Scratch tracks */
  struct stringlist scratch;

  /** @brief Maximum number of recent tracks to record in history */
  long history;

  /** @brief Expiry limit for noticed.db */
  long noticed_history;
  
  /** @brief User for server to run as */
  const char *user;

  /** @brief Nice value for rescan subprocess */
  long nice_rescan;

  /** @brief Paths to search for plugins */
  struct stringlist plugins;

  /** @brief List of stopwords */
  struct stringlist stopword;

  /** @brief List of collections */
  struct collectionlist collection;

  /** @brief Database checkpoint byte limit */
  long checkpoint_kbyte;

  /** @brief Databsase checkpoint minimum */
  long checkpoint_min;

  /** @brief Path to mixer device */
  char *mixer;

  /** @brief Mixer channel to use */
  char *channel;

  /** @brief Secondary listen address */
  struct netaddress listen;

  /** @brief Alias format string */
  const char *alias;

  /** @brief Nice value for server */
  long nice_server;

  /** @brief Nice value for speaker */
  long nice_speaker;

  /** @brief Command execute by speaker to play audio */
  const char *speaker_command;

  /** @brief Pause mode for command backend */
  const char *pause_mode;
  
  /** @brief Target sample format */
  struct stream_header sample_format;

  /** @brief Sox syntax generation */
  long sox_generation;

  /** @brief API used to play sound */
  const char *api;

  /** @brief Maximum size of a playlist */
  long playlist_max;

  /** @brief Maximum lifetime of a playlist lock */
  long playlist_lock_timeout;

#if !_WIN32
  /** @brief Home directory for state files */
  const char *home;
#endif

  /** @brief Login username */
  char *username;

  /** @brief Login password */
  char *password;

  /** @brief Address to connect to */
  struct netaddress connect;

  /** @brief Directories to search for web templates */
  struct stringlist templates;

  /** @brief Canonical URL of web interface */
  char *url;

  /** @brief Short display limit */
  long short_display;

  /** @brief Maximum refresh interval for web interface (seconds) */
  long refresh;

  /** @brief Minimum refresh interval for web interface (seconds) */
  long refresh_min;

  /** @brief Target queue length */
  long queue_pad;

  /** @brief Minimum time between a track being played again */
  long replay_min;
  
  struct namepartlist namepart;		/* transformations */

  /** @brief Termination signal for subprocesses */
  int signal;

  /** @brief ALSA output device */
  const char *device;

  struct transformlist transform;	/* path name transformations */

  /** @brief Address to send audio data to */
  struct netaddress broadcast;

  /** @brief Source address for network audio transmission */
  struct netaddress broadcast_from;

  /** @brief RTP delay threshold */
  long rtp_delay_threshold;

  /** @brief Whether to ignore the server's suggested RTP arrangement and
   * always request a unicast stream */
  int rtp_always_request;

  /** @brief RTP buffer low-water mark */
  long rtp_minbuffer;

  /** @brief RTP buffer maximum size */
  long rtp_maxbuffer;

  /** @brief RTP receive buffer size */
  long rtp_rcvbuf;

  /** @brief Fixed RTP listening address */
  struct netaddress rtp_request_address;

  /** @brief @c disorder-playrtp instance name (for naming sockets etc.) */
  char *rtp_instance_name;

  /** @brief Verbose RTP transmission logging */
  int rtp_verbose;
  
  /** @brief TTL for multicast packets */
  long multicast_ttl;

  /** @brief Whether to loop back multicast packets */
  int multicast_loop;

  /** @brief Maximum size of RTP payload to send
   *
   * This is the maximum number of bytes we pass to write(2); to determine
   * actual packet sizes, add a UDP header and an IP header (and a link layer
   * header if it's the link layer size you care about).
   *
   * Don't make this too big or arithmetic will start to overflow.
   */
  long rtp_max_payload;

  /** @brief Whether to allow MTU discovery
   *
   * This is `yes' to force it on, `no' to force it off, or `default' to do
   * whatever the system is configured to do.  Note that this only has a
   * useful effect in IPv4, since IPv6 doesn't permit hop-by-hop
   * fragmentation.
   */
  char *rtp_mtu_discovery;

  /** @brief Login lifetime in seconds */
  long cookie_login_lifetime;

  /** @brief Signing key lifetime in seconds */
  long cookie_key_lifetime;

  /** @brief Default rights for a new user */
  char *default_rights;

  /** @brief Path to sendmail executable */
  char *sendmail;

  /** @brief SMTP server for sending mail */
  char *smtp_server;

  /** @brief Origin address for outbound mail */
  char *mail_sender;

  /** @brief Maximum number of tracks in response to 'new' */
  long new_max;

  /** @brief Minimum interval between password reminder emails */
  long reminder_interval;

  /** @brief Whether to allow user management over TCP */
  int remote_userman;

  /** @brief Maximum age of biased-up tracks */
  long new_bias_age;

  /** @brief Maximum bias */
  long new_bias;

  /** @brief Rescan on (un)mount */
  int mount_rescan;

  /** @brief RTP mode */
  const char *rtp_mode;

  /** @brief HLS support master switch */
  int hls_enable;

  /** @brief HLS base URLs, one per collection root */
  struct urlmap_list hls_urlmap;

  /* derived values: */
  int nparts;				/* number of distinct name parts */
  char **parts;				/* name part list  */

  /* undocumented, for testing only */
  long dbversion;
};

extern struct config *config;
/* the current configuration */

int config_read(int server,
                const struct config *oldconfig);
/* re-read config, return 0 on success or non-0 on error.
 * Only updates @config@ if the new configuration is valid. */

char *config_get_file2(struct config *c, const char *name);
char *config_get_file(const char *name);
/* get a filename within the home directory */

struct passwd;

char *config_usersysconf(const struct passwd *pw );
/* get the user's conffile in /etc */

char *config_private(void);
/* get the private config file */

int config_verify(void);

void config_free(struct config *c);

extern char *configfile, *userconfigfile;
extern int config_per_user;

extern const struct uaudio *const *config_uaudio_apis;

/** @brief Returns the URL base for the given collection, or NULL if not found */
const char* urlmap_for(struct urlmap_list *map, const char* collection);

#endif /* CONFIGURATION_H */

/*
Local Variables:
c-basic-offset:2
comment-column:40
End:
*/
