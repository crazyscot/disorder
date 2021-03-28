/*
 * This file is part of DisOrder.
 * Copyright (C) 2004-2011, 2013 Richard Kettlewell
 * Portions copyright (C) 2007 Mark Wooding
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
/** @file lib/configuration.c
 * @brief Configuration file support
 */

#include "common.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#if HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <ctype.h>
#include <stddef.h>
#if HAVE_PWD_H
# include <pwd.h>
#endif
#if HAVE_LANGINFO_H
# include <langinfo.h>
#endif

#include <signal.h>

#include "rights.h"
#include "configuration.h"
#include "mem.h"
#include "log.h"
#include "split.h"
#include "syscalls.h"
#include "home.h"
#include "table.h"
#include "inputline.h"
#include "charset.h"
#include "defs.h"
#include "printf.h"
#include "regexp.h"
#include "regsub.h"
#include "signame.h"
#include "authhash.h"
#include "vector.h"
#if !_WIN32
#include "uaudio.h"
#endif

/** @brief Path to config file 
 *
 * set_configfile() sets the default if it is null.
 */
char *configfile;

/** @brief Path to user's config file
 *
 * set_configfile() sets the default if it is null.
 */
char *userconfigfile;

/** @brief Read user configuration
 *
 * If clear, the user-specific configuration is not read.
 */
int config_per_user = 1;

#if !_WIN32
/** @brief Table of audio APIs
 *
 * Only set in server processes.
 */
const struct uaudio *const *config_uaudio_apis;
#endif

/** @brief Config file parser state */
struct config_state {
  /** @brief Filename */
  const char *path;

  /** @brief Line number */
  int line;

  /** @brief Configuration object under construction */
  struct config *config;
};

/** @brief Current configuration */
struct config *config;

/** @brief One configuration item */
struct conf {
  /** @brief Name as it appears in the config file */
  const char *name;

  /** @brief Offset in @ref config structure */
  size_t offset;

  /** @brief Pointer to item type */
  const struct conftype *type;

  /** @brief Pointer to item-specific validation routine
   * @param cs Configuration state
   * @param nvec Length of (proposed) new value
   * @param vec Elements of new value
   * @return 0 on success, non-0 on error
   *
   * The validate function should report any error it detects.
   */
  int (*validate)(const struct config_state *cs,
		  int nvec, char **vec);
};

/** @brief Type of a configuration item */
struct conftype {
  /** @brief Pointer to function to set item
   * @param cs Configuration state
   * @param whoami Configuration item to set
   * @param nvec Length of new value
   * @param vec New value
   * @return 0 on success, non-0 on error
   */
  int (*set)(const struct config_state *cs,
	     const struct conf *whoami,
	     int nvec, char **vec);

  /** @brief Pointer to function to free item
   * @param c Configuration structure to free an item of
   * @param whoami Configuration item to free
   */
  void (*free)(struct config *c, const struct conf *whoami);
};

/** @brief Compute the address of an item */
#define ADDRESS(C, TYPE) ((TYPE *)((char *)(C) + whoami->offset))
/** @brief Return the value of an item */
#define VALUE(C, TYPE) (*ADDRESS(C, TYPE))

static int stringlist_compare(const struct stringlist *a,
                              const struct stringlist *b);
static int namepartlist_compare(const struct namepartlist *a,
                                const struct namepartlist *b);

static int set_signal(const struct config_state *cs,
		      const struct conf *whoami,
		      int nvec, char **vec) {
  int n;
  
  if(nvec != 1) {
    disorder_error(0, "%s:%d: '%s' requires one argument",
	  cs->path, cs->line, whoami->name);
    return -1;
  }
  if((n = find_signal(vec[0])) == -1) {
    disorder_error(0, "%s:%d: unknown signal '%s'",
	  cs->path, cs->line, vec[0]);
    return -1;
  }
  VALUE(cs->config, int) = n;
  return 0;
}

static int set_collections(const struct config_state *cs,
			   const struct conf *whoami,
			   int nvec, char **vec) {
  struct collectionlist *cl;
  const char *root, *encoding, *module;
  
  switch(nvec) {
  case 1:
    module = 0;
    encoding = 0;
    root = vec[0];
    break;
  case 2:
    module = vec[0];
    encoding = 0;
    root = vec[1];
    break;
  case 3:
    module = vec[0];
    encoding = vec[1];
    root = vec[2];
    break;
  case 0:
    disorder_error(0, "%s:%d: '%s' requires at least one argument",
		   cs->path, cs->line, whoami->name);
    return -1;
  default:
    disorder_error(0, "%s:%d: '%s' requires at most three arguments",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  /* Sanity check root */
  if(root[0] != '/') {
    disorder_error(0, "%s:%d: collection root must start with '/'",
		   cs->path, cs->line);
    return -1;
  }
  if(root[1] && root[strlen(root)-1] == '/') {
    disorder_error(0, "%s:%d: collection root must not end with '/'",
		   cs->path, cs->line);
    return -1;
  }
  /* Defaults */
  if(!module)
    module = "fs";
#if HAVE_LANGINFO_H
  if(!encoding)
    encoding = nl_langinfo(CODESET);
#else
  if(!encoding)
    encoding = "ascii";
#endif
  cl = ADDRESS(cs->config, struct collectionlist);
  ++cl->n;
  cl->s = xrealloc(cl->s, cl->n * sizeof (struct collection));
  cl->s[cl->n - 1].module = xstrdup(module);
  cl->s[cl->n - 1].encoding = xstrdup(encoding);
  cl->s[cl->n - 1].root = xstrdup(root);
  return 0;
}

static int set_boolean(const struct config_state *cs,
		       const struct conf *whoami,
		       int nvec, char **vec) {
  int state;
  
  if(nvec != 1) {
    disorder_error(0, "%s:%d: '%s' takes only one argument",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  if(!strcmp(vec[0], "yes")) state = 1;
  else if(!strcmp(vec[0], "no")) state = 0;
  else {
    disorder_error(0, "%s:%d: argument to '%s' must be 'yes' or 'no'",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  VALUE(cs->config, int) = state;
  return 0;
}

static int set_string(const struct config_state *cs,
		      const struct conf *whoami,
		      int nvec, char **vec) {
  if(nvec != 1) {
    disorder_error(0, "%s:%d: '%s' takes only one argument",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  xfree(VALUE(cs->config, char *));
  VALUE(cs->config, char *) = xstrdup(vec[0]);
  return 0;
}

static int set_integer(const struct config_state *cs,
		       const struct conf *whoami,
		       int nvec, char **vec) {
  char *e;

  if(nvec != 1) {
    disorder_error(0, "%s:%d: '%s' takes only one argument",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  if(xstrtol(ADDRESS(cs->config, long), vec[0], &e, 0)) {
    disorder_error(errno, "%s:%d: converting integer", cs->path, cs->line);
    return -1;
  }
  if(*e) {
    disorder_error(0, "%s:%d: invalid integer syntax", cs->path, cs->line);
    return -1;
  }
  return 0;
}

static int set_stringlist_accum(const struct config_state *cs,
				const struct conf *whoami,
				int nvec, char **vec) {
  int n;
  struct stringlist *s;
  struct stringlistlist *sll;

  sll = ADDRESS(cs->config, struct stringlistlist);
  if(nvec == 0) {
    sll->n = 0;
    return 0;
  }
  sll->n++;
  sll->s = xrealloc(sll->s, (sll->n * sizeof (struct stringlist)));
  s = &sll->s[sll->n - 1];
  s->n = nvec;
  s->s = xmalloc((nvec + 1) * sizeof (char *));
  for(n = 0; n < nvec; ++n)
    s->s[n] = xstrdup(vec[n]);
  return 0;
}

static int set_string_accum(const struct config_state *cs,
			    const struct conf *whoami,
			    int nvec, char **vec) {
  int n;
  struct stringlist *sl;

  sl = ADDRESS(cs->config, struct stringlist);
  if(nvec == 0) {
    sl->n = 0;
    return 0;
  }
  for(n = 0; n < nvec; ++n) {
    sl->n++;
    sl->s = xrealloc(sl->s, (sl->n * sizeof (char *)));
    sl->s[sl->n - 1] = xstrdup(vec[n]);
  }
  return 0;
}

static int parse_sample_format(const struct config_state *cs,
			       struct stream_header *format,
			       int nvec, char **vec) {
  char *p = vec[0];
  long t;

  if(nvec != 1) {
    disorder_error(0, "%s:%d: wrong number of arguments", cs->path, cs->line);
    return -1;
  }
  if(xstrtol(&t, p, &p, 0)) {
    disorder_error(errno, "%s:%d: converting bits-per-sample",
		   cs->path, cs->line);
    return -1;
  }
  if(t != 8 && t != 16) {
    disorder_error(0, "%s:%d: bad bits-per-sample (%ld)",
		   cs->path, cs->line, t);
    return -1;
  }
  if(format) format->bits = (uint8_t)t;
  switch (*p) {
    case 'l': case 'L': t = ENDIAN_LITTLE; p++; break;
    case 'b': case 'B': t = ENDIAN_BIG; p++; break;
    default: t = ENDIAN_NATIVE; break;
  }
  if(format) format->endian = (uint8_t)t;
  if(*p != '/') {
    disorder_error(errno, "%s:%d: expected `/' after bits-per-sample",
	  cs->path, cs->line);
    return -1;
  }
  p++;
  if(xstrtol(&t, p, &p, 0)) {
    disorder_error(errno, "%s:%d: converting sample-rate", cs->path, cs->line);
    return -1;
  }
  if(t < 1 || t > INT_MAX) {
    disorder_error(0, "%s:%d: silly sample-rate (%ld)", cs->path, cs->line, t);
    return -1;
  }
  if(format) format->rate = t;
  if(*p != '/') {
    disorder_error(0, "%s:%d: expected `/' after sample-rate",
		   cs->path, cs->line);
    return -1;
  }
  p++;
  if(xstrtol(&t, p, &p, 0)) {
    disorder_error(errno, "%s:%d: converting channels", cs->path, cs->line);
    return -1;
  }
  if(t < 1 || t > 8) {
    disorder_error(0, "%s:%d: silly number (%ld) of channels",
		   cs->path, cs->line, t);
    return -1;
  }
  if(format) format->channels = (uint8_t)t;
  if(*p) {
    disorder_error(0, "%s:%d: junk after channels", cs->path, cs->line);
    return -1;
  }
  return 0;
}

static int set_sample_format(const struct config_state *cs,
			     const struct conf *whoami,
			     int nvec, char **vec) {
  return parse_sample_format(cs, ADDRESS(cs->config, struct stream_header),
			     nvec, vec);
}

static int set_namepart(const struct config_state *cs,
			const struct conf *whoami,
			int nvec, char **vec) {
  struct namepartlist *npl = ADDRESS(cs->config, struct namepartlist);
  unsigned reflags;
  regexp *re;
  char errstr[RXCERR_LEN];
  size_t erroffset;
  int n;

  if(nvec < 3) {
    disorder_error(0, "%s:%d: namepart needs at least 3 arguments",
		   cs->path, cs->line);
    return -1;
  }
  if(nvec > 5) {
    disorder_error(0, "%s:%d: namepart needs at most 5 arguments",
		   cs->path, cs->line);
    return -1;
  }
  reflags = nvec >= 5 ? regsub_flags(vec[4]) : 0;
  if(!(re = regexp_compile(vec[1], regsub_compile_options(reflags),
			   errstr, sizeof(errstr), &erroffset)))
  {
    disorder_error(0, "%s:%d: compiling regexp /%s/: %s (offset %zu)",
		   cs->path, cs->line, vec[1], errstr, erroffset);
    return -1;
  }
  npl->s = xrealloc(npl->s, (npl->n + 1) * sizeof (struct namepart));
  npl->s[npl->n].part = xstrdup(vec[0]);
  npl->s[npl->n].re = re;
  npl->s[npl->n].res = xstrdup(vec[1]);
  npl->s[npl->n].replace = xstrdup(vec[2]);
  npl->s[npl->n].context = xstrdup(vec[3]);
  npl->s[npl->n].reflags = reflags;
  ++npl->n;
  /* XXX a bit of a bodge; relies on there being very few parts. */
  for(n = 0; (n < cs->config->nparts
	      && strcmp(cs->config->parts[n], vec[0])); ++n)
    ;
  if(n >= cs->config->nparts) {
    cs->config->parts = xrealloc(cs->config->parts,
				 (cs->config->nparts + 1) * sizeof (char *));
    cs->config->parts[cs->config->nparts++] = xstrdup(vec[0]);
  }
  return 0;
}

static int set_transform(const struct config_state *cs,
			 const struct conf *whoami,
			 int nvec, char **vec) {
  struct transformlist *tl = ADDRESS(cs->config, struct transformlist);
  regexp *re;
  char errstr[RXCERR_LEN];
  unsigned reflags;
  size_t erroffset;

  if(nvec < 3) {
    disorder_error(0, "%s:%d: transform needs at least 3 arguments",
		   cs->path, cs->line);
    return -1;
  }
  if(nvec > 5) {
    disorder_error(0, "%s:%d: transform needs at most 5 arguments",
		   cs->path, cs->line);
    return -1;
  }
  reflags = (nvec >= 5 ? regsub_flags(vec[4]) : 0);
  if(!(re = regexp_compile(vec[1], regsub_compile_options(reflags),
			   errstr, sizeof(errstr), &erroffset)))
  {
    disorder_error(0, "%s:%d: compiling regexp /%s/: %s (offset %zu)",
		   cs->path, cs->line, vec[1], errstr, erroffset);
    return -1;
  }
  tl->t = xrealloc(tl->t, (tl->n + 1) * sizeof (struct namepart));
  tl->t[tl->n].type = xstrdup(vec[0]);
  tl->t[tl->n].context = xstrdup(vec[3] ? vec[3] : "*");
  tl->t[tl->n].re = re;
  tl->t[tl->n].replace = xstrdup(vec[2]);
  tl->t[tl->n].flags = reflags;
  ++tl->n;
  return 0;
}

static int set_urlmap(const struct config_state *cs,
		      const struct conf *whoami,
		      int nvec, char **vec) {
  struct urlmap_list *uml = ADDRESS(cs->config, struct urlmap_list);
  if(nvec != 2) {
    disorder_error(0, "%s:%d: '%s' requires two arguments",
		   cs->path, cs->line, whoami->name);
    return -1;
  }

  uml->m = xrealloc(uml->m, (uml->n+1) * sizeof(struct urlmap));
  struct urlmap *new1 = & uml->m[uml->n];
  ++uml->n;
  new1->key = xstrdup(vec[0]); // unvalidated
  new1->url = xstrdup(vec[1]);
  return 0;
}

static int set_rights(const struct config_state *cs,
		      const struct conf *whoami,
		      int nvec, char **vec) {
  if(nvec != 1) {
    disorder_error(0, "%s:%d: '%s' requires one argument",
		   cs->path, cs->line, whoami->name);
    return -1;
  }
  if(parse_rights(vec[0], 0, 1)) {
    disorder_error(0, "%s:%d: invalid rights string '%s'",
		   cs->path, cs->line, vec[0]);
    return -1;
  }
  return set_string(cs, whoami, nvec, vec);
}

static int set_netaddress(const struct config_state *cs,
			  const struct conf *whoami,
			  int nvec, char **vec) {
  struct netaddress *na = ADDRESS(cs->config, struct netaddress);

  if(netaddress_parse(na, nvec, vec)) {
    disorder_error(0, "%s:%d: invalid network address", cs->path, cs->line);
    return -1;
  }
  return 0;
}

/* free functions */

static void free_none(struct config attribute((unused)) *c,
		      const struct conf attribute((unused)) *whoami) {
}

static void free_string(struct config *c,
			const struct conf *whoami) {
  xfree(VALUE(c, char *));
  VALUE(c, char *) = 0;
}

static void free_stringlist(struct config *c,
			    const struct conf *whoami) {
  int n;
  struct stringlist *sl = ADDRESS(c, struct stringlist);

  for(n = 0; n < sl->n; ++n)
    xfree(sl->s[n]);
  xfree(sl->s);
}

static void free_stringlistlist(struct config *c,
				const struct conf *whoami) {
  int n, m;
  struct stringlistlist *sll = ADDRESS(c, struct stringlistlist);
  struct stringlist *sl;

  for(n = 0; n < sll->n; ++n) {
    sl = &sll->s[n];
    for(m = 0; m < sl->n; ++m)
      xfree(sl->s[m]);
    xfree(sl->s);
  }
  xfree(sll->s);
}

static void free_collectionlist(struct config *c,
				const struct conf *whoami) {
  struct collectionlist *cll = ADDRESS(c, struct collectionlist);
  struct collection *cl;
  int n;

  for(n = 0; n < cll->n; ++n) {
    cl = &cll->s[n];
    xfree(cl->module);
    xfree(cl->encoding);
    xfree(cl->root);
  }
  xfree(cll->s);
}

static void free_namepartlist(struct config *c,
			      const struct conf *whoami) {
  struct namepartlist *npl = ADDRESS(c, struct namepartlist);
  struct namepart *np;
  int n;

  for(n = 0; n < npl->n; ++n) {
    np = &npl->s[n];
    xfree(np->part);
    regexp_free(np->re);
    xfree(np->res);
    xfree(np->replace);
    xfree(np->context);
  }
  xfree(npl->s);
}

static void free_transformlist(struct config *c,
			       const struct conf *whoami) {
  struct transformlist *tl = ADDRESS(c, struct transformlist);
  struct transform *t;
  int n;

  for(n = 0; n < tl->n; ++n) {
    t = &tl->t[n];
    xfree(t->type);
    regexp_free(t->re);
    xfree(t->replace);
    xfree(t->context);
  }
  xfree(tl->t);
}

static void free_netaddress(struct config *c,
			    const struct conf *whoami) {
  struct netaddress *na = ADDRESS(c, struct netaddress);

  xfree(na->address);
}

static void free_urlmap_list(struct config *c,
			       const struct conf *whoami) {
  struct urlmap_list *ul = ADDRESS(c, struct urlmap_list);
  for (int n=0; n<ul->n; ++n) {
    struct urlmap *map = &ul->m[n];
    if (map) {
      xfree(map->url);
      xfree(map->key);
    }
  }
  xfree(ul->m);
  ul->m = 0;
}


/* configuration types */

static const struct conftype
  type_signal = { set_signal, free_none },
  type_collections = { set_collections, free_collectionlist },
  type_boolean = { set_boolean, free_none },
  type_string = { set_string, free_string },
  type_integer = { set_integer, free_none },
  type_stringlist_accum = { set_stringlist_accum, free_stringlistlist },
  type_string_accum = { set_string_accum, free_stringlist },
  type_sample_format = { set_sample_format, free_none },
  type_namepart = { set_namepart, free_namepartlist },
  type_transform = { set_transform, free_transformlist },
  type_netaddress = { set_netaddress, free_netaddress },
  type_rights = { set_rights, free_string },
  type_hls_url = { set_urlmap, free_urlmap_list };

/* specific validation routine */

/** @brief Perform a test on a filename
 * @param test Test function to call on mode bits
 * @param what Type of file sought
 *
 * If @p test returns 0 then the file is not a @p what and an error
 * is reported and -1 is returned.
 */
#define VALIDATE_FILE(test, what) do {			\
  struct stat sb;					\
  int n;						\
							\
  for(n = 0; n < nvec; ++n) {				\
    if(stat(vec[n], &sb) < 0) {				\
      disorder_error(errno, "%s:%d: %s",		\
                     cs->path, cs->line, vec[n]);	\
      return -1;					\
    }							\
    if(!test(sb.st_mode)) {				\
      disorder_error(0, "%s:%d: %s is not a %s",	\
	             cs->path, cs->line, vec[n], what);	\
      return -1;					\
    }							\
  }							\
} while(0)

/** @brief Validate an absolute path
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_isabspath(const struct config_state *cs,
			      int nvec, char **vec) {
  int n;

  for(n = 0; n < nvec; ++n)
    if(vec[n][0] != '/') {
      disorder_error(0, "%s:%d: %s: not an absolute path", 
		     cs->path, cs->line, vec[n]);
      return -1;
    }
  return 0;
}

/** @brief Validate an existing directory
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_isdir(const struct config_state *cs,
			  int nvec, char **vec) {
  VALIDATE_FILE(S_ISDIR, "directory");
  return 0;
}

/** @brief Validate an existing regular file
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_isreg(const struct config_state *cs,
			  int nvec, char **vec) {
  VALIDATE_FILE(S_ISREG, "regular file");
  return 0;
}

/** @brief Validate a player pattern
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_player(const struct config_state *cs,
			   int nvec,
			   char attribute((unused)) **vec) {
  if(nvec && nvec < 2) {
    disorder_error(0, "%s:%d: should be at least 'player PATTERN MODULE'",
		   cs->path, cs->line);
    return -1;
  }
  return 0;
}

/** @brief Validate a track length pattern
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_tracklength(const struct config_state *cs,
				int nvec,
				char attribute((unused)) **vec) {
  if(nvec && nvec < 2) {
    disorder_error(0, "%s:%d: should be at least 'tracklength PATTERN MODULE'",
		   cs->path, cs->line);
    return -1;
  }
  return 0;
}

/** @brief Common code for validating integer values
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @param n_out Where to put the value
 */
static int common_validate_integer(const struct config_state *cs,
				   int nvec, char **vec, long *n_out) {
  char errbuf[1024];

  if(nvec < 1) {
    disorder_error(0, "%s:%d: missing argument", cs->path, cs->line);
    return -1;
  }
  if(nvec > 1) {
    disorder_error(0, "%s:%d: too many arguments", cs->path, cs->line);
    return -1;
  }
  if(xstrtol(n_out, vec[0], 0, 0)) {
    disorder_error(0, "%s:%d: %s", cs->path, cs->line,
                   format_error(ec_errno, errno, errbuf, sizeof errbuf));
    return -1;
  }
  return 0;
}

/** @brief Validate a non-negative (@c long) integer
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_non_negative(const struct config_state *cs,
				 int nvec, char **vec) {
  long n;
  if(common_validate_integer(cs, nvec, vec, &n)) return -1;
  if(n < 0) {
    disorder_error(0, "%s:%d: must not be negative", cs->path, cs->line);
    return -1;
  }
  return 0;
}

/** @brief Validate a positive (@c long) integer
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_positive(const struct config_state *cs,
			  int nvec, char **vec) {
  long n;
  if(common_validate_integer(cs, nvec, vec, &n)) return -1;
  if(n <= 0) {
    disorder_error(0, "%s:%d: must be positive", cs->path, cs->line);
    return -1;
  }
  return 0;
}

#if !_WIN32
/** @brief Validate a system username
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_isauser(const struct config_state *cs,
			    int attribute((unused)) nvec,
			    char **vec) {
  if(!getpwnam(vec[0])) {
    disorder_error(0, "%s:%d: no such user as '%s'", cs->path, cs->line, vec[0]);
    return -1;
  }
  return 0;
}
#endif

/** @brief Validate a sample format string
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_sample_format(const struct config_state *cs,
				  int attribute((unused)) nvec,
				  char **vec) {
  return parse_sample_format(cs, 0, nvec, vec);
}

/** @brief Validate anything
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0
 */
static int validate_any(const struct config_state attribute((unused)) *cs,
			int attribute((unused)) nvec,
			char attribute((unused)) **vec) {
  return 0;
}

/** @brief Validate a URL
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 *
 * Rather cursory.
 */
static int validate_url(const struct config_state attribute((unused)) *cs,
			int attribute((unused)) nvec,
			char **vec) {
  const char *s;
  int n;
  /* absoluteURI   = scheme ":" ( hier_part | opaque_part )
     scheme        = alpha *( alpha | digit | "+" | "-" | "." ) */
  s = vec[0];
  n = strspn(s, ("abcdefghijklmnopqrstuvwxyz"
		 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		 "0123456789"));
  if(s[n] != ':') {
    disorder_error(0, "%s:%d: invalid url '%s'", cs->path, cs->line, vec[0]);
    return -1;
  }
  if(!strncmp(s, "http:", 5)
     || !strncmp(s, "https:", 6)) {
    s += n + 1;
    /* we only do a rather cursory check */
    if(strncmp(s, "//", 2)) {
      disorder_error(0, "%s:%d: invalid url '%s'", cs->path, cs->line, vec[0]);
      return -1;
    }
  }
  return 0;
}

/** @brief Validate a URL map entry
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 *
 * Rather cursory.
 */
static int validate_urlmap(const struct config_state attribute((unused)) *cs,
			int attribute((unused)) nvec,
			char **vec) {
  return validate_url(cs, 0, &vec[1]);
}

/** @brief Validate an alias pattern
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_alias(const struct config_state *cs,
			  int nvec,
			  char **vec) {
  const char *s;
  int in_brackets = 0, c;

  if(nvec < 1) {
    disorder_error(0, "%s:%d: missing argument", cs->path, cs->line);
    return -1;
  }
  if(nvec > 1) {
    disorder_error(0, "%s:%d: too many arguments", cs->path, cs->line);
    return -1;
  }
  s = vec[0];
  while((c = (unsigned char)*s++)) {
    if(in_brackets) {
      if(c == '}')
	in_brackets = 0;
      else if(!isalnum(c)) {
	disorder_error(0, "%s:%d: invalid part name in alias expansion in '%s'",
		       cs->path, cs->line, vec[0]);
	  return -1;
      }
    } else {
      if(c == '{') {
	in_brackets = 1;
	if(*s == '/')
	  ++s;
      } else if(c == '\\') {
	if(!(c = (unsigned char)*s++)) {
	  disorder_error(0, "%s:%d: unterminated escape in alias expansion in '%s'",
			 cs->path, cs->line, vec[0]);
	  return -1;
	} else if(c != '\\' && c != '{') {
	  disorder_error(0, "%s:%d: invalid escape in alias expansion in '%s'",
			 cs->path, cs->line, vec[0]);
	  return -1;
	}
      }
    }
    ++s;
  }
  if(in_brackets) {
    disorder_error(0,
		   "%s:%d: unterminated part name in alias expansion in '%s'",
		   cs->path, cs->line, vec[0]);
    return -1;
  }
  return 0;
}

/** @brief Validate a hash algorithm name
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_algo(const struct config_state attribute((unused)) *cs,
			 int nvec,
			 char **vec) {
  if(nvec != 1) {
    disorder_error(0, "%s:%d: invalid algorithm specification", cs->path, cs->line);
    return -1;
  }
  if(!valid_authhash(vec[0])) {
    disorder_error(0, "%s:%d: unsuported algorithm '%s'", cs->path, cs->line, vec[0]);
    return -1;
  }
  return 0;
}

#if !_WIN32
/** @brief Validate a playback backend name
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_backend(const struct config_state attribute((unused)) *cs,
                            int nvec,
                            char **vec) {
  int n;
  if(nvec != 1) {
    disorder_error(0, "%s:%d: invalid sound API specification", cs->path, cs->line);
    return -1;
  }
  if(!strcmp(vec[0], "network")) {
    disorder_error(0, "'api network' is deprecated; use 'api rtp'");
    return 0;
  }
  if(config_uaudio_apis) {
    for(n = 0; config_uaudio_apis[n]; ++n)
      if(!strcmp(vec[0], config_uaudio_apis[n]->name))
        return 0;
    disorder_error(0, "%s:%d: unrecognized sound API '%s'", cs->path, cs->line, vec[0]);
    return -1;
  }
  /* In non-server processes we have no idea what's valid */
  return 0;
}
#endif

/** @brief Validate a pause mode string
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_pausemode(const struct config_state attribute((unused)) *cs,
                              int nvec,
                              char **vec) {
  if(nvec == 1 && (!strcmp(vec[0], "silence") || !strcmp(vec[0], "suspend")))
    return 0;
  disorder_error(0, "%s:%d: invalid pause mode", cs->path, cs->line);
  return -1;
}

/** @brief Validate an MTU-discovery setting
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 */
static int validate_mtu_discovery(const struct config_state attribute((unused)) *cs,
				  int nvec,
				  char **vec) {
  if (nvec == 1 &&
      (!strcmp(vec[0], "default") ||
       !strcmp(vec[0], "yes") ||
       !strcmp(vec[0], "no")))
    return 0;
  disorder_error(0, "%s:%d: invalid MTU-discovery setting", cs->path, cs->line);
  return -1;
}

/** @brief Validate a destination network address
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 *
 * By a destination address, it is meant that it must not be a wildcard
 * address.
 */
static int validate_destaddr(const struct config_state attribute((unused)) *cs,
			     int nvec,
			     char **vec) {
  struct netaddress na[1];

  if(netaddress_parse(na, nvec, vec)) {
    disorder_error(0, "%s:%d: invalid network address", cs->path, cs->line);
    return -1;
  }
  if(!na->address) {
    disorder_error(0, "%s:%d: destination address required", cs->path, cs->line);
    return -1;
  }
  xfree(na->address);
  return 0;
}

/** @brief Validate an internet address
 * @param cs Configuration state
 * @param nvec Length of (proposed) new value
 * @param vec Elements of new value
 * @return 0 on success, non-0 on error
 *
 * By a destination address, it is meant that it must be either IPv4 or IPv6.
 */
static int validate_inetaddr(const struct config_state *cs,
			     int nvec, char **vec) {
  struct netaddress na[1];

  if(netaddress_parse(na, nvec, vec)) {
    disorder_error(0, "%s:%d: invalid network address", cs->path, cs->line);
    return -1;
  }
  switch(na->af) {
    case AF_INET: case AF_INET6: case AF_UNSPEC: break;
    default:
      disorder_error(0, "%s:%d: must be an intenet address",
		     cs->path, cs->line);
      return -1;
  }
  return 0;
}

/** @brief Item name and and offset */
#define C(x) #x, offsetof(struct config, x)
/** @brief Item name and and offset */
#define C2(x,y) #x, offsetof(struct config, y)

/** @brief All configuration items */
static const struct conf conf[] = {
  { C(alias),            &type_string,           validate_alias },
#if !_WIN32
  { C(api),              &type_string,           validate_backend },
#endif
  { C(authorization_algorithm), &type_string,    validate_algo },
  { C(broadcast),        &type_netaddress,       validate_destaddr },
  { C(broadcast_from),   &type_netaddress,       validate_any },
  { C(channel),          &type_string,           validate_any },
  { C(checkpoint_kbyte), &type_integer,          validate_non_negative },
  { C(checkpoint_min),   &type_integer,          validate_non_negative },
  { C(collection),       &type_collections,      validate_any },
  { C(connect),          &type_netaddress,       validate_destaddr },
  { C(cookie_key_lifetime),  &type_integer,      validate_positive },
  { C(cookie_login_lifetime),  &type_integer,    validate_positive },
  { C(dbversion),        &type_integer,          validate_positive },
  { C(default_rights),   &type_rights,           validate_any },
  { C(device),           &type_string,           validate_any },
  { C(history),          &type_integer,          validate_positive },
  { C(hls_enable),       &type_boolean,          validate_any },
  { C(hls_urlmap),       &type_hls_url,          validate_urlmap },
#if !_WIN32
  { C(home),             &type_string,           validate_isabspath },
#endif
  { C(listen),           &type_netaddress,       validate_any },
  { C(mail_sender),      &type_string,           validate_any },
  { C(mixer),            &type_string,           validate_any },
  { C(mount_rescan),     &type_boolean,          validate_any },
  { C(multicast_loop),   &type_boolean,          validate_any },
  { C(multicast_ttl),    &type_integer,          validate_non_negative },
  { C(namepart),         &type_namepart,         validate_any },
  { C(new_bias),         &type_integer,          validate_positive },
  { C(new_bias_age),     &type_integer,          validate_positive },
  { C(new_max),          &type_integer,          validate_positive },
  { C2(nice, nice_rescan), &type_integer,        validate_non_negative },
  { C(nice_rescan),      &type_integer,          validate_non_negative },
  { C(nice_server),      &type_integer,          validate_any },
  { C(nice_speaker),     &type_integer,          validate_any },
  { C(noticed_history),  &type_integer,          validate_positive },
  { C(password),         &type_string,           validate_any },
  { C(pause_mode),       &type_string,           validate_pausemode },
  { C(player),           &type_stringlist_accum, validate_player },
  { C(playlist_lock_timeout), &type_integer,     validate_positive },
  { C(playlist_max) ,    &type_integer,          validate_positive },
  { C(plugins),          &type_string_accum,     validate_isdir },
  { C(queue_pad),        &type_integer,          validate_positive },
  { C(refresh),          &type_integer,          validate_positive },
  { C(refresh_min),      &type_integer,          validate_non_negative },
  { C(reminder_interval), &type_integer,         validate_positive },
  { C(remote_userman),   &type_boolean,          validate_any },
  { C(replay_min),       &type_integer,          validate_non_negative },
  { C(rtp_always_request), &type_boolean,	 validate_any },
  { C(rtp_delay_threshold), &type_integer,       validate_positive },
  { C(rtp_instance_name), &type_string,		 validate_any },
  { C(rtp_max_payload),	 &type_integer,		 validate_positive },
  { C(rtp_maxbuffer),	 &type_integer,		 validate_non_negative },
  { C(rtp_minbuffer),	 &type_integer,		 validate_non_negative },
  { C(rtp_mode),         &type_string,           validate_any },
  { C(rtp_mtu_discovery), &type_string,		 validate_mtu_discovery },
  { C(rtp_rcvbuf),	 &type_integer,		 validate_non_negative },
  { C(rtp_request_address), &type_netaddress,	 validate_inetaddr },
  { C(rtp_verbose),      &type_boolean,          validate_any },
  { C(sample_format),    &type_sample_format,    validate_sample_format },
  { C(scratch),          &type_string_accum,     validate_isreg },
#if !_WIN32
  { C(sendmail),         &type_string,           validate_isabspath },
#endif
  { C(short_display),    &type_integer,          validate_positive },
  { C(signal),           &type_signal,           validate_any },
  { C(smtp_server),      &type_string,           validate_any },
  { C(sox_generation),   &type_integer,          validate_non_negative },
#if !_WIN32
  { C2(speaker_backend, api),  &type_string,     validate_backend },
#endif
  { C(speaker_command),  &type_string,           validate_any },
  { C(stopword),         &type_string_accum,     validate_any },
  { C(templates),        &type_string_accum,     validate_isdir },
  { C(tracklength),      &type_stringlist_accum, validate_tracklength },
  { C(transform),        &type_transform,        validate_any },
  { C(url),              &type_string,           validate_url },
#if !_WIN32
  { C(user),             &type_string,           validate_isauser },
#endif
  { C(username),         &type_string,           validate_any },
};

/** @brief Find a configuration item's definition by key */
static const struct conf *find(const char *key) {
  int n;

  if((n = TABLE_FIND(conf, name, key)) < 0)
    return 0;
  return &conf[n];
}

/** @brief Set a new configuration value
 * @param cs Configuration state
 * @param nvec Length of @p vec
 * @param vec Name and new value
 * @return 0 on success, non-0 on error.
 *
 * @c vec[0] is the name, the rest is the value.
 */
static int config_set(const struct config_state *cs,
		      int nvec, char **vec) {
  const struct conf *which;

  D(("config_set %s", vec[0]));
  if(!(which = find(vec[0]))) {
    disorder_error(0, "%s:%d: unknown configuration key '%s'",
	  cs->path, cs->line, vec[0]);
    return -1;
  }
  return (which->validate(cs, nvec - 1, vec + 1)
	  || which->type->set(cs, which, nvec - 1, vec + 1));
}

/** @brief Set a configuration item from parameters
 * @param cs Configuration state
 * @param which Item to set
 * @param ... Value as strings, terminated by (char *)NULL
 * @return 0 on success, non-0 on error
 */
static int config_set_args(const struct config_state *cs,
			   const char *which, ...) {
  va_list ap;
  struct vector v[1];
  char *s;
  int rc;

  vector_init(v);
  vector_append(v, (char *)which);
  va_start(ap, which);
  while((s = va_arg(ap, char *)))
    vector_append(v, s);
  va_end(ap);
  vector_terminate(v);
  rc = config_set(cs, v->nvec, v->vec);
  xfree(v->vec);
  return rc;
}

/** @brief Error callback used by config_include()
 * @param msg Error message
 * @param u User data (@ref config_state)
 */
static void config_error(const char *msg, void *u) {
  const struct config_state *cs = u;

  disorder_error(0, "%s:%d: %s", cs->path, cs->line, msg);
}

/** @brief Include a file by name
 * @param c Configuration to update
 * @param path Path to read
 * @return 0 on success, non-0 on error
 */
static int config_include(struct config *c, const char *path) {
  FILE *fp;
  char *buffer, *inputbuffer, **vec;
  int n, ret = 0;
  struct config_state cs;

  cs.path = path;
  cs.line = 0;
  cs.config = c;
  D(("%s: reading configuration", path));
  if(!(fp = fopen(path, "r"))) {
    disorder_error(errno, "error opening %s", path);
    return -1;
  }
  while(!inputline(path, fp, &inputbuffer, '\n')) {
    ++cs.line;
    if(!(buffer = mb2utf8(inputbuffer))) {
      disorder_error(errno, "%s:%d: cannot convert to UTF-8", cs.path, cs.line);
      ret = -1;
      xfree(inputbuffer);
      continue;
    }
    xfree(inputbuffer);
    if(!(vec = split(buffer, &n, SPLIT_COMMENTS|SPLIT_QUOTES,
		     config_error, &cs))) {
      ret = -1;
      xfree(buffer);
      continue;
    }
    if(n) {
      /* 'include' is special-cased */
      if(!strcmp(vec[0], "include")) {
	if(n != 2) {
	  disorder_error(0, "%s:%d: must be 'include PATH'", cs.path, cs.line);
	  ret = -1;
	} else
	  config_include(c, vec[1]);
      } else
	ret |= config_set(&cs, n, vec);
    }
    for(n = 0; vec[n]; ++n) xfree(vec[n]);
    xfree(vec);
    xfree(buffer);
  }
  if(ferror(fp)) {
    disorder_error(errno, "error reading %s", path);
    ret = -1;
  }
  fclose(fp);
  return ret;
}

/** @brief Default stopword setting */
static const char *const default_stopwords[] = {
  "stopword",

  "01",
  "02",
  "03",
  "04",
  "05",
  "06",
  "07",
  "08",
  "09",
  "1",
  "10",
  "11",
  "12",
  "13",
  "14",
  "15",
  "16",
  "17",
  "18",
  "19",
  "2",
  "20",
  "21",
  "22",
  "23",
  "24",
  "25",
  "26",
  "27",
  "28",
  "29",
  "3",
  "30",
  "4",
  "5",
  "6",
  "7",
  "8",
  "9",
  "a",
  "am",
  "an",
  "and",
  "as",
  "for",
  "i",
  "im",
  "in",
  "is",
  "of",
  "on",
  "the",
  "to",
  "too",
  "we",
};
#define NDEFAULT_STOPWORDS (sizeof default_stopwords / sizeof *default_stopwords)

/** @brief Default player patterns */
static const char *const default_players[] = {
  "*.ogg",
  "*.flac",
  "*.mp3",
  "*.wav",
};
#define NDEFAULT_PLAYERS (sizeof default_players / sizeof *default_players)

/** @brief Make a new default configuration
 * @return New configuration
 */
static struct config *config_default(void) {
  struct config *c = xmalloc(sizeof *c);
#if !_WIN32
  const char *logname;
  struct passwd *pw;
#endif
  struct config_state cs;
  size_t n;

  cs.path = "<internal>";
  cs.line = 0;
  cs.config = c;
  /* Strings had better be xstrdup'd as they will get freed at some point. */
  c->history = 60;
#if !_WIN32
  c->home = xstrdup(pkgstatedir);
#endif
#if _WIN32
  {
    char buffer[128];
    DWORD bufsize = sizeof buffer;
    if(!GetUserNameA(buffer, &bufsize))
      disorder_fatal(0, "cannot determine our username");
    c->username = xstrdup(buffer);
  }
#else
  if(!(pw = getpwuid(getuid())))
    disorder_fatal(0, "cannot determine our username");
  logname = pw->pw_name;
  c->username = xstrdup(logname);
#endif
  c->refresh = 15;
  c->refresh_min = 1;
#ifdef SIGKILL
  c->signal = SIGKILL;
#else
  c->signal = SIGTERM;
#endif
  c->alias = xstrdup("{/artist}{/album}{/title}{ext}");
  c->device = xstrdup("default");
  c->nice_rescan = 10;
  c->speaker_command = 0;
  c->sample_format.bits = 16;
  c->sample_format.rate = 44100;
  c->sample_format.channels = 2;
  c->sample_format.endian = ENDIAN_NATIVE;
  c->queue_pad = 10;
  c->replay_min = 8 * 3600;
  c->api = NULL;
  c->multicast_ttl = 1;
  c->multicast_loop = 1;
  c->authorization_algorithm = xstrdup("sha1");
  c->noticed_history = 31;
  c->short_display = 32;
  c->mixer = 0;
  c->channel = 0;
  c->dbversion = 2;
  c->cookie_login_lifetime = 86400;
  c->cookie_key_lifetime = 86400 * 7;
#if !_WIN32
  if(sendmail_binary[0] && strcmp(sendmail_binary, "none"))
    c->sendmail = xstrdup(sendmail_binary);
#endif
  c->smtp_server = xstrdup("127.0.0.1");
  c->new_max = 100;
  c->reminder_interval = 600;		/* 10m */
  c->new_bias_age = 7 * 86400;		/* 1 week */
  c->new_bias = 4500000;		/* 50 times the base weight */
  c->sox_generation = DEFAULT_SOX_GENERATION;
  c->playlist_max = INT_MAX;            /* effectively no limit */
  c->playlist_lock_timeout = 10;        /* 10s */
  c->mount_rescan = 1;
  /* Default stopwords */
  if(config_set(&cs, (int)NDEFAULT_STOPWORDS, (char **)default_stopwords))
    exit(1);
  /* Default player configuration */
  for(n = 0; n < NDEFAULT_PLAYERS; ++n) {
    if(config_set_args(&cs, "player",
		       default_players[n], "execraw", "disorder-decode", (char *)0))
      exit(1);
    if(config_set_args(&cs, "tracklength",
		       default_players[n], "disorder-tracklength", (char *)0))
      exit(1);
  }
  c->broadcast.af = -1;
  c->broadcast_from.af = -1;
  c->listen.af = -1;
  c->connect.af = -1;
  c->rtp_mode = xstrdup("auto");
  c->rtp_max_payload = -1;
  c->rtp_mtu_discovery = xstrdup("default");
  return c;
}

#if !_WIN32
/** @brief Construct a filename
 * @param c Configuration
 * @param name Base filename
 * @return Full filename
 *
 * Usually use config_get_file() instead.
 */
char *config_get_file2(struct config *c, const char *name) {
  char *s;

  byte_xasprintf(&s, "%s/%s", c->home, name);
  return s;
}
#endif

/** @brief Set the default configuration file */
static void set_configfile(void) {
  char *t;

#if !_WIN32
  if(!configfile) {
    configfile = getenv("DISORDER_CONFIG");
    if(!configfile)
      byte_xasprintf(&configfile, "%s/config", pkgconfdir);
  }
#endif
  if(!userconfigfile && config_per_user) {
    if((t = getenv("DISORDER_USERCONFIG"))) userconfigfile = xstrdup(t);
    else if(!(userconfigfile = profile_filename("passwd")))
      disorder_fatal(0, "failed to find user profile directory");
  }
}

/** @brief Free a configuration object
 * @param c Configuration to free
 *
 * @p c is indeterminate after this function is called.
 */
void config_free(struct config *c) {
  int n;

  if(c) {
    for(n = 0; n < (int)(sizeof conf / sizeof *conf); ++n)
      conf[n].type->free(c, &conf[n]);
    for(n = 0; n < c->nparts; ++n)
      xfree(c->parts[n]);
    xfree(c->parts);
    xfree(c);
  }
}

/** @brief Set post-parse defaults
 * @param c Configuration to update
 * @param server True when running in the server
 *
 * If @p server is set then certain parts of the configuration are more
 * strictly validated.
 */
static void config_postdefaults(struct config *c,
				int server) {
  struct config_state cs;
  const struct conf *whoami;
  int n;

  static const char *namepart[][4] = {
    { "title",  "/([0-9]+ *[-:]? *)?([^/]+)\\.[a-zA-Z0-9]+$", "$2", "display" },
    { "title",  "/([^/]+)\\.[a-zA-Z0-9]+$",           "$1", "sort" },
    { "album",  "/([^/]+)/[^/]+$",                    "$1", "*" },
    { "artist", "/([^/]+)/[^/]+/[^/]+$",              "$1", "*" },
    { "ext",    "(\\.[a-zA-Z0-9]+)$",                 "$1", "*" },
  };
#define NNAMEPART (int)(sizeof namepart / sizeof *namepart)

  static const char *transform[][5] = {
    { "track", "^.*/([0-9]+ *[-:]? *)?([^/]+)\\.[a-zA-Z0-9]+$", "$2", "display", "" },
    { "track", "^.*/([^/]+)\\.[a-zA-Z0-9]+$",           "$1", "sort", "" },
    { "dir",   "^.*/([^/]+)$",                          "$1", "*", "" },
    { "dir",   "^(the) ([^/]*)",                        "$2, $1", "sort", "i", },
    { "dir",   "[[:punct:]]",                           "", "sort", "g", }
  };
#define NTRANSFORM (int)(sizeof transform / sizeof *transform)

  cs.path = "<internal>";
  cs.line = 0;
  cs.config = c;
  if(!c->namepart.n) {
    whoami = find("namepart");
    for(n = 0; n < NNAMEPART; ++n)
      set_namepart(&cs, whoami, 4, (char **)namepart[n]);
  }
  if(!c->transform.n) {
    whoami = find("transform");
    for(n = 0; n < NTRANSFORM; ++n)
      set_transform(&cs, whoami, 5, (char **)transform[n]);
  }
  if(!c->api) {
    if(c->speaker_command)
      c->api = xstrdup("command");
    else if(c->broadcast.af != -1)
      c->api = xstrdup("rtp");
#if !_WIN32
    else if(config_uaudio_apis)
      c->api = xstrdup(uaudio_default(config_uaudio_apis,
                                      UAUDIO_API_SERVER)->name);
#endif
    else
      c->api = xstrdup("<none>");
  }
  if(!strcmp(c->api, "network"))
    c->api = xstrdup("rtp");
  if(server) {
    if(!strcmp(c->api, "command") && !c->speaker_command)
      disorder_fatal(0, "'api command' but speaker_command is not set");
    if((!strcmp(c->api, "rtp")) &&
       c->broadcast.af == -1 && strcmp(c->rtp_mode, "request"))
      disorder_fatal(0, "'api rtp' but broadcast is not set "
		     "and mode is not not 'request'");
  }
  /* Override sample format */
  if(!strcmp(c->api, "rtp")) {
    c->sample_format.rate = 44100;
    c->sample_format.channels = 2;
    c->sample_format.bits = 16;
    c->sample_format.endian = ENDIAN_NATIVE;
  }
  if(!strcmp(c->api, "coreaudio")) {
    c->sample_format.rate = 44100;
    c->sample_format.channels = 2;
    c->sample_format.bits = 16;
    c->sample_format.endian = ENDIAN_NATIVE;
  }
  if(!c->default_rights) {
    rights_type r = RIGHTS__MASK & ~(RIGHT_ADMIN|RIGHT_REGISTER
				     |RIGHT_MOVE__MASK
				     |RIGHT_SCRATCH__MASK
				     |RIGHT_REMOVE__MASK);
    r |= RIGHT_SCRATCH_ANY|RIGHT_MOVE_ANY|RIGHT_REMOVE_ANY;
    c->default_rights = rights_string(r);
  }
}

/** @brief (Re-)read the config file
 * @param server If set, do extra checking
 * @param oldconfig Old configuration for compatibility check
 * @return 0 on success, non-0 on error
 *
 * If @p oldconfig is set, then certain compatibility checks are done between
 * the old and new configurations.
 */
int config_read(int server,
                const struct config *oldconfig) {
  struct config *c;
  char *privconf;
  struct passwd *pw = NULL;

  set_configfile();
  c = config_default();
  /* standalone client installs might not have a global config file */
  if(configfile)
    if(access(configfile, F_OK) == 0)
      if(config_include(c, configfile))
        return -1;
  /* if we can read the private config file, do */
  if((privconf = config_private())
     && access(privconf, R_OK) == 0
     && config_include(c, privconf))
    return -1;
  xfree(privconf);
  /* if there's a per-user system config file for this user, read it */
  if(config_per_user) {
#if !_WIN32
    if(!(pw = getpwuid(getuid())))
      disorder_fatal(0, "cannot determine our username");
    if((privconf = config_usersysconf(pw))
       && access(privconf, F_OK) == 0
       && config_include(c, privconf))
      return -1;
    xfree(privconf);
#endif
    /* if we have a password file, read it */
    if(access(userconfigfile, F_OK) == 0
       && config_include(c, userconfigfile))
      return -1;
  }
  /* install default namepart and transform settings */
  config_postdefaults(c, server);
  if(oldconfig)  {
    int failed = 0;
#if !_WIN32
    if(strcmp(c->home, oldconfig->home)) {
      disorder_error(0, "'home' cannot be changed without a restart");
      failed = 1;
    }
#endif
    if(strcmp(c->alias, oldconfig->alias)) {
      disorder_error(0, "'alias' cannot be changed without a restart");
      failed = 1;
    }
    if(strcmp(c->user, oldconfig->user)) {
      disorder_error(0, "'user' cannot be changed without a restart");
      failed = 1;
    }
    if(c->nice_speaker != oldconfig->nice_speaker) {
      disorder_error(0, "'nice_speaker' cannot be changed without a restart");
      /* ...but we accept the new config anyway */
    }
    if(c->nice_server != oldconfig->nice_server) {
      disorder_error(0, "'nice_server' cannot be changed without a restart");
      /* ...but we accept the new config anyway */
    }
    if(namepartlist_compare(&c->namepart, &oldconfig->namepart)) {
      disorder_error(0, "'namepart' settings cannot be changed without a restart");
      failed = 1;
    }
    if(stringlist_compare(&c->stopword, &oldconfig->stopword)) {
      disorder_error(0, "'stopword' settings cannot be changed without a restart");
      failed = 1;
    }
    if(failed) {
      disorder_error(0, "not installing incompatible new configuration");
      return -1;
    }
  }
  /* everything is good so we shall use the new config */
  config_free(config);
  /* warn about obsolete directives */
  config = c;
  return 0;
}

/** @brief Return the path to the private configuration file */
char *config_private(void) {
#if _WIN32
  return NULL;
#else
  char *s;

  if((s = getenv("DISORDER_PRIVCONFIG"))) return xstrdup(s);
  set_configfile();
  byte_xasprintf(&s, "%s.private", configfile);
  return s;
#endif
}

#if !_WIN32
/** @brief Return the path to user-specific system configuration */
char *config_usersysconf(const struct passwd *pw) {
  char *s;

  set_configfile();
  if((s = getenv("DISORDER_USERCONFIG_SYS")))
    return xstrdup(s);
  else if(!strchr(pw->pw_name, '/')) {
    byte_xasprintf(&s, "%s.%s", configfile, pw->pw_name);
    return s;
  } else
    return 0;
}

/** @brief Get a filename within the home directory
 * @param name Relative name
 * @return Full path
 */
char *config_get_file(const char *name) {
  return config_get_file2(config, name);
}
#endif

/** @brief Order two stringlists
 * @param a First stringlist
 * @param b Second stringlist
 * @return <0, 0 or >0 if a<b, a=b or a>b
 */
static int stringlist_compare(const struct stringlist *a,
                              const struct stringlist *b) {
  int n = 0, c;

  while(n < a->n && n < b->n) {
    if((c = strcmp(a->s[n], b->s[n])))
      return c;
    ++n;
  }
  if(a->n < b->n)
    return -1;
  else if(a->n > b->n)
    return 1;
  else
    return 0;
}

/** @brief Order two namepart definitions
 * @param a First namepart definition
 * @param b Second namepart definition
 * @return <0, 0 or >0 if a<b, a=b or a>b
 */
static int namepart_compare(const struct namepart *a,
                            const struct namepart *b) {
  int c;

  if((c = strcmp(a->part, b->part)))
    return c;
  if((c = strcmp(a->res, b->res)))
    return c;
  if((c = strcmp(a->replace, b->replace)))
    return c;
  if((c = strcmp(a->context, b->context)))
    return c;
  if(a->reflags > b->reflags)
    return 1;
  if(a->reflags < b->reflags)
    return -1;
  return 0;
}

/** @brief Order two lists of namepart definitions
 * @param a First list of namepart definitions
 * @param b Second list of namepart definitions
 * @return <0, 0 or >0 if a<b, a=b or a>b
 */
static int namepartlist_compare(const struct namepartlist *a,
                                const struct namepartlist *b) {
  int n = 0, c;

  while(n < a->n && n < b->n) {
    if((c = namepart_compare(&a->s[n], &b->s[n])))
      return c;
    ++n;
  }
  if(a->n > b->n)
    return 1;
  else if(a->n < b->n)
    return -1;
  else
    return 0;
}

/** @brief Verify configuration table.
 * @return The number of problems found
*/
int config_verify(void) {
  int fails = 0;
  size_t n;
  for(n = 1; n < sizeof conf / sizeof *conf; ++n)
    if(strcmp(conf[n-1].name, conf[n].name) >= 0) {
      fprintf(stderr, "%s >= %s\n", conf[n-1].name, conf[n].name);
      ++fails;
    }
  return fails;
}

/** @brief Returns the URL base for the given collection, or NULL if not found */
const char* urlmap_for(struct urlmap_list *map, const char* collection) {
  for (int i=0; i<map->n; ++i) {
    if (0==strncmp(collection, map->m[i].key, strlen(collection)))
      return map->m[i].url;
  }
  return 0;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
End:
*/
