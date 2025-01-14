/*
 * This file is part of DisOrder.
 * Copyright (C) 2004-13 Richard Kettlewell
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
/** @file lib/client.c
 * @brief Simple C client
 *
 * See @ref lib/eclient.c for an asynchronous-capable client
 * implementation.
 */

#include "common.h"

#include <sys/types.h>
#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#if HAVE_SYS_UN_H
# include <sys/un.h>
#endif
#if HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <errno.h>
#if HAVE_NETDB_H
# include <netdb.h>
#endif

#include "log.h"
#include "mem.h"
#include "queue.h"
#include "client.h"
#include "charset.h"
#include "hex.h"
#include "split.h"
#include "vector.h"
#include "inputline.h"
#include "kvp.h"
#include "syscalls.h"
#include "printf.h"
#include "sink.h"
#include "addr.h"
#include "authhash.h"
#include "client-common.h"
#include "rights.h"
#include "kvp.h"
#include "socketio.h"

/** @brief Client handle contents */
struct disorder_client {
  /** @brief Stream to read from */
  struct source *input;
  /** @brief Stream to write to */
  struct sink *output;
  /** @brief Peer description */
  char *ident;
  /** @brief Username */
  char *user;
  /** @brief Report errors to @c stderr */
  int verbose;
  /** @brief Last error string */
  const char *last;
  /** @brief Address family */
  int family;
  /** @brief True if open */
  int open;
  /** @brief Socket I/O context */
  struct socketio sio;
  /** @brief Whether to try to open a privileged connection */
  int trypriv;
};

/** @brief Create a new client
 * @param verbose If nonzero, write extra junk to stderr
 * @return Pointer to new client
 *
 * You must call disorder_connect(), disorder_connect_user() or
 * disorder_connect_cookie() to connect it.  Use disorder_close() to
 * dispose of the client when finished with it.
 */
disorder_client *disorder_new(int verbose) {
  disorder_client *c = xmalloc(sizeof (struct disorder_client));

  c->verbose = verbose;
  c->family = -1;
  c->trypriv = 1;
  return c;
}

/** @brief Don't try to make a privileged connection
 * @param c Client
 *
 * You must call this before any of the connection functions (e.g.,
 * disorder_connect(), disorder_connect_user()), if at all.
 */
void disorder_force_unpriv(disorder_client *c) {
  assert(!c->open);
  c->trypriv = 0;
}

/** @brief Determine the local socket address of this client */
int disorder_client_sockname(disorder_client *c,
			     struct sockaddr *sa, socklen_t *len_inout) {
  int rc;
  if((rc = getsockname(c->sio.sd, sa, len_inout)))
    disorder_error(errno, "failed to read client socket name");
  return rc;
}

/** @brief Determine the remote peer address for this client */
int disorder_client_peername(disorder_client *c,
			     struct sockaddr *sa, socklen_t *len_inout) {
  int rc;
  if((rc = getpeername(c->sio.sd, sa, len_inout)))
    disorder_error(errno, "failed to read client socket name");
  return rc;
}

/** @brief Read a response line
 * @param c Client
 * @param rp Where to store response, or NULL (UTF-8)
 * @return Response code 0-999 or -1 on error
 */
static int response(disorder_client *c, char **rp) {
  char *r;
  char errbuf[1024];

  if(inputlines(c->ident, c->input, &r, '\n')) {
    byte_xasprintf((char **)&c->last, "input error: %s",
                   format_error(c->input->eclass, source_err(c->input), errbuf, sizeof errbuf));
    return -1;
  }
  D(("response: %s", r));
  if(rp)
    *rp = r;
  if(r[0] >= '0' && r[0] <= '9'
     && r[1] >= '0' && r[1] <= '9'
     && r[2] >= '0' && r[2] <= '9'
     && r[3] == ' ') {
    c->last = r + 4;
    return (r[0] * 10 + r[1]) * 10 + r[2] - 111 * '0';
  } else {
    c->last = "invalid reply format";
    disorder_error(0, "invalid reply format from %s", c->ident);
    return -1;
  }
}

/** @brief Return last response string
 * @param c Client
 * @return Last response string (UTF-8, English) or NULL
 */
const char *disorder_last(disorder_client *c) {
  return c->last;
}

/** @brief Read and partially parse a response
 * @param c Client
 * @param rp Where to store response text (or NULL) (UTF-8)
 * @return 0 on success, non-0 on error
 *
 * 5xx responses count as errors.
 *
 * @p rp will NOT be filled in for xx9 responses (where it is just
 * commentary for a command where it would normally be meaningful).
 *
 * NB that the response will NOT be converted to the local encoding.
 */
static int check_response(disorder_client *c, char **rp) {
  int rc;
  char *r;

  if((rc = response(c, &r)) == -1)
    return -1;
  else if(rc / 100 == 2) {
    if(rp)
      *rp = (rc % 10 == 9) ? 0 : xstrdup(r + 4);
    xfree(r);
    return 0;
  } else {
    if(c->verbose)
      disorder_error(0, "from %s: %s", c->ident, utf82mb(r));
    xfree(r);
    return rc;
  }
}

/** @brief Issue a command and parse a simple response
 * @param c Client
 * @param rp Where to store result, or NULL
 * @param cmd Command
 * @param ap Arguments (UTF-8), terminated by (char *)0
 * @return 0 on success, non-0 on error
 *
 * 5xx responses count as errors.
 *
 * @p rp will NOT be filled in for xx9 responses (where it is just
 * commentary for a command where it would normally be meaningful).
 *
 * NB that the response will NOT be converted to the local encoding
 * nor will quotes be stripped.  See dequote().
 *
 * Put @ref disorder__body in the argument list followed by a char **
 * and int giving the body to follow the command.  If the int is @c -1
 * then the list is assumed to be NULL-terminated.  This may be used
 * only once.
 *
 * Put @ref disorder__list in the argument list followed by a char **
 * and int giving a list of arguments to include.  If the int is @c -1
 * then the list is assumed to be NULL-terminated.  This may be used
 * any number of times.
 *
 * Put @ref disorder__integer in the argument list followed by a long to
 * send its value in decimal.  This may be used any number of times.
 *
 * Put @ref disorder__time in the argument list followed by a time_t
 * to send its value in decimal.  This may be used any number of
 * times.
 *
 * Usually you would call this via one of the following interfaces:
 * - disorder_simple()
 */
static int disorder_simple_v(disorder_client *c,
			     char **rp,
			     const char *cmd,
                             va_list ap) {
  const char *arg;
  struct dynstr d;
  char **body = NULL;
  int nbody = 0;
  int has_body = 0;
  char errbuf[1024];

  if(!c->open) {
    c->last = "not connected";
    disorder_error(0, "not connected to server");
    return -1;
  }
  if(cmd) {
    dynstr_init(&d);
    dynstr_append_string(&d, cmd);
    while((arg = va_arg(ap, const char *))) {
      if(arg == disorder__body) {
	body = va_arg(ap, char **);
	nbody = va_arg(ap, int);
	has_body = 1;
      } else if(arg == disorder__list) {
	char **list = va_arg(ap, char **);
	int nlist = va_arg(ap, int);
        int n;
	if(nlist < 0) {
	  for(nlist = 0; list[nlist]; ++nlist)
	    ;
	}
	for(n = 0; n < nlist; ++n) {
	  dynstr_append(&d, ' ');
	  dynstr_append_string(&d, quoteutf8(arg));
	}
      } else if(arg == disorder__integer) {
	long n = va_arg(ap, long);
	char buffer[16];
	byte_snprintf(buffer, sizeof buffer, "%ld", n);
	dynstr_append(&d, ' ');
	dynstr_append_string(&d, buffer);
      } else if(arg == disorder__time) {
	time_t n = va_arg(ap, time_t);
	char buffer[16];
	byte_snprintf(buffer, sizeof buffer, "%lld", (long long)n);
	dynstr_append(&d, ' ');
	dynstr_append_string(&d, buffer);
      } else {
	dynstr_append(&d, ' ');
	dynstr_append_string(&d, quoteutf8(arg));
      }
    }
    dynstr_append(&d, '\n');
    dynstr_terminate(&d);
    D(("command: %s", d.vec));
    if(sink_write(c->output, d.vec, d.nvec) < 0)
      goto write_error;
    xfree(d.vec);
    if(has_body) {
      int n;
      if(nbody < 0)
        for(nbody = 0; body[nbody]; ++nbody)
          ;
      for(n = 0; n < nbody; ++n) {
        if(body[n][0] == '.')
          if(sink_writec(c->output, '.') < 0)
            goto write_error;
        if(sink_writes(c->output, body[n]) < 0)
          goto write_error;
        if(sink_writec(c->output, '\n') < 0)
          goto write_error;
      }
      if(sink_writes(c->output, ".\n") < 0)
        goto write_error;
    }
    if(sink_flush(c->output))
      goto write_error;
  }
  return check_response(c, rp);
write_error:
  byte_xasprintf((char **)&c->last, "write error: %s", 
                 format_error(c->output->eclass, sink_err(c->output), errbuf, sizeof errbuf));
  disorder_error(0, "%s: %s", c->ident, c->last);
  return -1;
}

/** @brief Issue a command and parse a simple response
 * @param c Client
 * @param rp Where to store result, or NULL (UTF-8)
 * @param cmd Command
 * @return 0 on success, non-0 on error
 *
 * The remaining arguments are command arguments, terminated by (char
 * *)0.  They should be in UTF-8.
 *
 * 5xx responses count as errors.
 *
 * @p rp will NOT be filled in for xx9 responses (where it is just
 * commentary for a command where it would normally be meaningful).
 *
 * NB that the response will NOT be converted to the local encoding
 * nor will quotes be stripped.  See dequote().
 */
static int disorder_simple(disorder_client *c,
			   char **rp,
			   const char *cmd, ...) {
  va_list ap;
  int ret;

  va_start(ap, cmd);
  ret = disorder_simple_v(c, rp, cmd, ap);
  va_end(ap);
  return ret;
}

/** @brief Issue a command and split the response
 * @param c Client
 * @param vecp Where to store results
 * @param nvecp Where to store count of results
 * @param expected Expected count (or -1 to not check)
 * @param cmd Command
 * @return 0 on success, non-0 on error
 *
 * The remaining arguments are command arguments, terminated by (char
 * *)0.  They should be in UTF-8.
 *
 * 5xx responses count as errors.
 *
 * @p rp will NOT be filled in for xx9 responses (where it is just
 * commentary for a command where it would normally be meaningful).
 *
 * NB that the response will NOT be converted to the local encoding
 * nor will quotes be stripped.  See dequote().
 */
static int disorder_simple_split(disorder_client *c,
				 char ***vecp,
				 int *nvecp,
				 int expected,
				 const char *cmd, ...) {
  va_list ap;
  int ret;
  char *r;
  char **vec;
  int nvec;

  va_start(ap, cmd);
  ret = disorder_simple_v(c, &r, cmd, ap);
  va_end(ap);
  if(!ret) {
    vec = split(r, &nvec, SPLIT_QUOTES, 0, 0);
    xfree(r);
    if(expected < 0 || nvec == expected) {
      *vecp = vec;
      *nvecp = nvec;
    } else {
      disorder_error(0, "malformed reply to %s", cmd);
      c->last = "malformed reply";
      ret = -1;
      free_strings(nvec, vec);
    }
  }
  if(ret) {
    *vecp = NULL;
    *nvecp = 0;
  }
  return ret;
}

/** @brief Dequote a result string
 * @param rc 0 on success, non-0 on error
 * @param rp Where result string is stored (UTF-8)
 * @return @p rc
 *
 * This is used as a wrapper around disorder_simple() to dequote
 * results in place.
 */
static int dequote(int rc, char **rp) {
  char **rr;

  if(!rc) {
    if((rr = split(*rp, 0, SPLIT_QUOTES, 0, 0)) && *rr) {
      xfree(*rp);
      *rp = *rr;
      xfree(rr);
      return 0;
    }
    disorder_error(0, "invalid reply: %s", *rp);
  }
  return rc;
}

/** @brief Generic connection routine
 * @param conf Configuration to follow
 * @param c Client
 * @param username Username to log in with or NULL
 * @param password Password to log in with or NULL
 * @param cookie Cookie to log in with or NULL
 * @return 0 on success, non-0 on error
 *
 * @p cookie is tried first if not NULL.  If it is NULL then @p
 * username must not be.  If @p username is not NULL then nor may @p
 * password be.
 */
int disorder_connect_generic(struct config *conf,
                             disorder_client *c,
                             const char *username,
                             const char *password,
                             const char *cookie) {
  SOCKET sd = INVALID_SOCKET;
  int nrvec = 0, rc;
  unsigned char *nonce = NULL;
  size_t nl;
  char *res = NULL;
  char *r = NULL, **rvec = NULL;
  const char *protocol, *algorithm, *challenge;
  struct sockaddr *sa = NULL;
  socklen_t salen;
  char errbuf[1024];

  if((salen = disorder_find_server(conf,
				   (c->trypriv ? 0 : DISORDER_FS_NOTPRIV),
				   &sa, &c->ident)) == (socklen_t)-1)
    return -1;
  c->input = 0;
  c->output = 0;
  if((sd = socket(sa->sa_family, SOCK_STREAM, 0)) < 0) {
    byte_xasprintf((char **)&c->last, "socket: %s",
                   format_error(ec_socket, socket_error(), errbuf, sizeof errbuf));
    disorder_error(0, "%s", c->last);
    return -1;
  }
  c->family = sa->sa_family;
  if(connect(sd, sa, salen) < 0) {
    byte_xasprintf((char **)&c->last, "connect: %s",
                   format_error(ec_socket, socket_error(), errbuf, sizeof errbuf));
    disorder_error(0, "%s", c->last);
    goto error;
  }
  socketio_init(&c->sio, sd);
  c->open = 1;
  sd = INVALID_SOCKET;
  c->output = sink_socketio(&c->sio);
  c->input = source_socketio(&c->sio);
  if((rc = disorder_simple(c, &r, 0, (const char *)0)))
    goto error_rc;
  if(!(rvec = split(r, &nrvec, SPLIT_QUOTES, 0, 0)))
    goto error;
  if(nrvec != 3) {
    c->last = "cannot parse server greeting";
    disorder_error(0, "cannot parse server greeting %s", r);
    goto error;
  }
  protocol = rvec[0];
  if(strcmp(protocol, "2")) {
    c->last = "unknown protocol version";
    disorder_error(0, "unknown protocol version: %s", protocol);
    goto error;
  }
  algorithm = rvec[1];
  challenge = rvec[2];
  if(!(nonce = unhex(challenge, &nl)))
    goto error;
  if(cookie) {
    if(!dequote(disorder_simple(c, &c->user, "cookie", cookie, (char *)0),
		&c->user))
      return 0;				/* success */
    if(!username) {
      c->last = "cookie failed and no username";
      disorder_error(0, "cookie did not work and no username available");
      goto error;
    }
  }
  if(!(res = authhash(nonce, nl, password, algorithm))) {
    c->last = "error computing authorization hash";
    goto error;
  }
  if((rc = disorder_simple(c, 0, "user", username, res, (char *)0)))
    goto error_rc;
  c->user = xstrdup(username);
  xfree(res);
  free_strings(nrvec, rvec);
  xfree(nonce);
  xfree(sa);
  xfree(r);
  return 0;
error:
  rc = -1;
error_rc:
  xfree(c->output);
  c->output = NULL;
  xfree(c->input);
  c->input = NULL;
  if(c->open) { socketio_close(&c->sio); c->open = 0; }
  if(sd != INVALID_SOCKET) closesocket(sd);
  return rc;
}

/** @brief Connect a client with a specified username and password
 * @param c Client
 * @param username Username to log in with
 * @param password Password to log in with
 * @return 0 on success, non-0 on error
 */
int disorder_connect_user(disorder_client *c,
			  const char *username,
			  const char *password) {
  return disorder_connect_generic(config,
                                  c,
				  username,
				  password,
				  0);
}

/** @brief Connect a client
 * @param c Client
 * @return 0 on success, non-0 on error
 *
 * The connection will use the username and password found in @ref
 * config, or directly from the database if no password is found and
 * the database is readable (usually only for root).
 */
int disorder_connect(disorder_client *c) {
  const char *username, *password;

  if(!(username = config->username)) {
    c->last = "no username";
    disorder_error(0, "no username configured");
    return -1;
  }
  password = config->password;
  /* If we're connecting as 'root' guess that we're the system root
   * user (or the jukebox user), both of which can use the privileged
   * socket.  They can also furtle with the db directly: that is why
   * privileged socket does not represent a privilege escalation. */
  if(!password
     && !strcmp(username, "root"))
    password = "anything will do for root";
  if(!password) {
    /* Oh well */
    c->last = "no password";
    disorder_error(0, "no password configured for user '%s'", username);
    return -1;
  }
  return disorder_connect_generic(config,
                                  c,
				  username,
				  password,
				  0);
}

/** @brief Connect a client
 * @param c Client
 * @param cookie Cookie to log in with, or NULL
 * @return 0 on success, non-0 on error
 *
 * If @p cookie is NULL or does not work then we attempt to log in as
 * guest instead (so when the cookie expires only an extra round trip
 * is needed rather than a complete new login).
 */
int disorder_connect_cookie(disorder_client *c,
			    const char *cookie) {
  return disorder_connect_generic(config,
                                  c,
				  "guest",
				  "",
				  cookie);
}

/** @brief Close a client
 * @param c Client
 * @return 0 on succcess, non-0 on errior
 *
 * The client is still closed even on error.  It might well be
 * appropriate to ignore the return value.
 */
int disorder_close(disorder_client *c) {
  int ret = 0;

  if(c->open)
    socketio_close(&c->sio);
  xfree(c->output);
  c->output = NULL;
  xfree(c->input);
  c->input = NULL;
  xfree(c->ident);
  c->ident = 0;
  xfree(c->user);
  c->user = 0;
  return ret;
}

static void client_error(const char *msg,
			 void attribute((unused)) *u) {
  disorder_error(0, "error parsing reply: %s", msg);
}

/** @brief Get a single queue entry
 * @param c Client
 * @param cmd Command
 * @param qp Where to store track information
 * @return 0 on success, non-0 on error
 */
static int onequeue(disorder_client *c, const char *cmd,
		    struct queue_entry **qp) {
  char *r;
  struct queue_entry *q;
  int rc;

  if((rc = disorder_simple(c, &r, cmd, (char *)0)))
    return rc;
  if(r) {
    q = xmalloc(sizeof *q);
    if(queue_unmarshall(q, r, client_error, 0))
      return -1;
    *qp = q;
  } else
    *qp = 0;
  return 0;
}

/** @brief Fetch the queue, recent list, etc */
static int readqueue(disorder_client *c,
		     struct queue_entry **qp) {
  struct queue_entry *qh, **qt = &qh, *q;
  char *l;
  char errbuf[1024];

  while(inputlines(c->ident, c->input, &l, '\n') >= 0) {
    if(!strcmp(l, ".")) {
      *qt = 0;
      *qp = qh;
      xfree(l);
      return 0;
    }
    q = xmalloc(sizeof *q);
    if(!queue_unmarshall(q, l, client_error, 0)) {
      *qt = q;
      qt = &q->next;
    }
    xfree(l);
  }
  if(source_err(c->input)) {
    byte_xasprintf((char **)&c->last, "input error: %s",
                   format_error(c->input->eclass, source_err(c->input), errbuf, sizeof errbuf));
  } else {
    c->last = "input error: unexpected EOF";
  }
  disorder_error(0, "%s: %s", c->ident, c->last);
  return -1;
}

/** @brief Read a dot-stuffed list
 * @param c Client
 * @param vecp Where to store list (UTF-8)
 * @param nvecp Where to store number of items, or NULL
 * @return 0 on success, non-0 on error
 *
 * The list will have a final NULL not counted in @p nvecp.
 */
static int readlist(disorder_client *c, char ***vecp, int *nvecp) {
  char *l;
  struct vector v;
  char errbuf[1024];

  vector_init(&v);
  while(inputlines(c->ident, c->input, &l, '\n') >= 0) {
    if(!strcmp(l, ".")) {
      vector_terminate(&v);
      if(nvecp)
	*nvecp = v.nvec;
      *vecp = v.vec;
      xfree(l);
      return 0;
    }
    vector_append(&v, xstrdup(l + (*l == '.')));
    xfree(l);
  }
  if(source_err(c->input)) {
    byte_xasprintf((char **)&c->last, "input error: %s",
                   format_error(c->input->eclass, source_err(c->input), errbuf, sizeof errbuf));
  } else {
    c->last = "input error: unexpxected EOF";
  }
  disorder_error(0, "%s: %s", c->ident, c->last);
  return -1;
}

/** @brief Return the user we logged in with
 * @param c Client
 * @return User name (owned by @p c, don't modify)
 */
char *disorder_user(disorder_client *c) {
  return c->user;
}

static void pairlist_error_handler(const char *msg,
			       void attribute((unused)) *u) {
  disorder_error(0, "error handling key-value pair reply: %s", msg);
}

/** @brief Get a list of key-value pairs
 * @param c Client
 * @param kp Where to store linked list of preferences
 * @param cmd Command
 * @param ... Arguments
 * @return 0 on success, non-0 on error
 */
static int pairlist(disorder_client *c, struct kvp **kp, const char *cmd, ...) {
  char **vec, **pvec;
  int nvec, npvec, n, rc;
  struct kvp *k;
  va_list ap;

  va_start(ap, cmd);
  rc = disorder_simple_v(c, 0, cmd, ap);
  va_end(ap);
  if(rc)
    return rc;
  if((rc = readlist(c, &vec, &nvec)))
     return rc;
  for(n = 0; n < nvec; ++n) {
    if(!(pvec = split(vec[n], &npvec, SPLIT_QUOTES, pairlist_error_handler, 0)))
      return -1;
    if(npvec != 2) {
      pairlist_error_handler("malformed response", 0);
      return -1;
    }
    *kp = k = xmalloc(sizeof *k);
    k->name = pvec[0];
    k->value = pvec[1];
    kp = &k->next;
    xfree(pvec);
  }
  free_strings(nvec, vec);
  *kp = 0;
  return 0;
}

#if _WIN32
# define boolean bodge_boolean
#endif

/** @brief Parse a boolean response
 * @param cmd Command for use in error messsage
 * @param value Result from server
 * @param flagp Where to store result
 * @return 0 on success, non-0 on error
 */
static int boolean(const char *cmd, const char *value,
		   int *flagp) {
  if(!strcmp(value, "yes")) *flagp = 1;
  else if(!strcmp(value, "no")) *flagp = 0;
  else {
    disorder_error(0, "malformed response to '%s'", cmd);
    return -1;
  }
  return 0;
}

/** @brief Log to a sink
 * @param c Client
 * @param s Sink to write log lines to
 * @return 0 on success, non-0 on error
 */
int disorder_log(disorder_client *c, struct sink *s) {
  char *l;
  int rc;
  char errbuf[1024];
    
  if((rc = disorder_simple(c, 0, "log", (char *)0)))
    return rc;
  while(inputlines(c->ident, c->input, &l, '\n') >= 0 && strcmp(l, "."))
    if(sink_printf(s, "%s\n", l) < 0) return -1;
  if(source_err(c->input)) {
    byte_xasprintf((char **)&c->last, "input error: %s",
		   format_error(c->input->eclass, source_err(c->input), errbuf, sizeof errbuf));
    return -1;
  } else if(source_eof(c->input)) {
    byte_xasprintf((char **)&c->last, "input error: unexpected EOF");
    return -1;
  }

  return 0;
}

#include "client-stubs.c"

/*
Local Variables:
c-basic-offset:2
comment-column:40
End:
*/
