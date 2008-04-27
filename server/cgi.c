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

#include <config.h>
#include "types.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <pcre.h>
#include <limits.h>
#include <fnmatch.h>
#include <ctype.h>

#include "mem.h"
#include "log.h"
#include "hex.h"
#include "charset.h"
#include "configuration.h"
#include "table.h"
#include "syscalls.h"
#include "kvp.h"
#include "vector.h"
#include "split.h"
#include "inputline.h"
#include "regsub.h"
#include "defs.h"
#include "sink.h"
#include "cgi.h"
#include "printf.h"
#include "mime.h"
#include "unicode.h"
#include "hash.h"

struct kvp *cgi_args;

/* options */
struct column {
  struct column *next;
  char *name;
  int ncolumns;
  char **columns;
};

/* macros */
struct cgi_macro {
  int nargs;
  char **args;
  const char *value;
};

static hash *cgi_macros;

/** @brief Parse of a template */
struct cgi_element {
  /** @brief Next element */
  struct cgi_element *next;

  /** @brief Element type */
  int type;
#define ELEMENT_TEXT 0
#define ELEMENT_EXPANSION 1

  /** @brief Line number at start of element */
  int line;
  
  /** @brief Plain text */
  char *text;

  /** @brief Expansion name */
  char *name;

  /** @brief Argument count */
  int nargs;

  /** @brief Argument values (NOT recursively expanded) */
  char **args;
};

#define RELIST(x) struct re *x, **x##_tail = &x

static int have_read_options;
static struct kvp *labels;
static struct column *columns;

static void include_options(const char *name);

static void cgi_parse_get(void) {
  const char *q;

  if(!(q = getenv("QUERY_STRING"))) fatal(0, "QUERY_STRING not set");
  cgi_args = kvp_urldecode(q, strlen(q));
}

static void cgi_input(char **ptrp, size_t *np) {
  const char *cl;
  char *q;
  size_t n, m = 0;
  int r;

  if(!(cl = getenv("CONTENT_LENGTH"))) fatal(0, "CONTENT_LENGTH not set");
  n = atol(cl);
  q = xmalloc_noptr(n + 1);
  while(m < n) {
    r = read(0, q + m, n - m);
    if(r > 0)
      m += r;
    else if(r == 0)
      fatal(0, "unexpected end of file reading request body");
    else switch(errno) {
    case EINTR: break;
    default: fatal(errno, "error reading request body");
    }
  }
  if(memchr(q, 0, n)) fatal(0, "null character in request body");
  q[n + 1] = 0;
  *ptrp = q;
  if(np) *np = n;
}

static int cgi_field_callback(const char *name, const char *value,
			      void *u) {
  char *disposition, *pname, *pvalue;
  char **namep = u;

  if(!strcmp(name, "content-disposition")) {
    if(mime_rfc2388_content_disposition(value,
					&disposition,
					&pname,
					&pvalue))
      fatal(0, "error parsing Content-Disposition field");
    if(!strcmp(disposition, "form-data")
       && pname
       && !strcmp(pname, "name")) {
      if(*namep)
	fatal(0, "duplicate Content-Disposition field");
      *namep = pvalue;
    }
  }
  return 0;
}

static int cgi_part_callback(const char *s,
			     void attribute((unused)) *u) {
  char *name = 0;
  struct kvp *k;
  
  if(!(s = mime_parse(s, cgi_field_callback, &name)))
    fatal(0, "error parsing part header");
  if(!name) fatal(0, "no name found");
  k = xmalloc(sizeof *k);
  k->next = cgi_args;
  k->name = name;
  k->value = s;
  cgi_args = k;
  return 0;
}

static void cgi_parse_multipart(const char *boundary) {
  char *q;
  
  cgi_input(&q, 0);
  if(mime_multipart(q, cgi_part_callback, boundary, 0))
    fatal(0, "invalid multipart object");
}

static void cgi_parse_post(void) {
  const char *ct, *boundary;
  char *q, *type;
  size_t n;
  struct kvp *k;

  if(!(ct = getenv("CONTENT_TYPE")))
    ct = "application/x-www-form-urlencoded";
  if(mime_content_type(ct, &type, &k))
    fatal(0, "invalid content type '%s'", ct);
  if(!strcmp(type, "application/x-www-form-urlencoded")) {
    cgi_input(&q, &n);
    cgi_args = kvp_urldecode(q, n);
    return;
  }
  if(!strcmp(type, "multipart/form-data")) {
    if(!(boundary = kvp_get(k, "boundary")))
      fatal(0, "no boundary parameter found");
    cgi_parse_multipart(boundary);
    return;
  }
  fatal(0, "unrecognized content type '%s'", type);
}

void cgi_parse(void) {
  const char *p;
  struct kvp *k;

  if(!(p = getenv("REQUEST_METHOD"))) fatal(0, "REQUEST_METHOD not set");
  if(!strcmp(p, "GET"))
    cgi_parse_get();
  else if(!strcmp(p, "POST"))
    cgi_parse_post();
  else
    fatal(0, "unknown request method %s", p);
  for(k = cgi_args; k; k = k->next)
    if(!utf8_valid(k->name, strlen(k->name))
       || !utf8_valid(k->value, strlen(k->value)))
      fatal(0, "invalid UTF-8 sequence in cgi argument");
}

const char *cgi_get(const char *name) {
  return kvp_get(cgi_args, name);
}

void cgi_output(cgi_sink *output, const char *fmt, ...) {
  va_list ap;
  int n;
  char *r;

  va_start(ap, fmt);
  n = byte_vasprintf(&r, fmt, ap);
  if(n < 0)
    fatal(errno, "error calling byte_vasprintf");
  if(output->quote)
    r = cgi_sgmlquote(r, 0);
  output->sink->write(output->sink, r, strlen(r));
  va_end(ap);
}

void cgi_header(struct sink *output, const char *name, const char *value) {
  sink_printf(output, "%s: %s\r\n", name, value);
}

void cgi_body(struct sink *output) {
  sink_printf(output, "\r\n");
}

char *cgi_sgmlquote(const char *s, int raw) {
  uint32_t *ucs, *p, c;
  char *b, *bp;
  int n;

  if(!raw) {
    if(!(ucs = utf8_to_utf32(s, strlen(s), 0))) exit(EXIT_FAILURE);
  } else {
    ucs = xmalloc_noptr((strlen(s) + 1) * sizeof(uint32_t));
    for(n = 0; s[n]; ++n)
      ucs[n] = (unsigned char)s[n];
    ucs[n] = 0;
  }

  n = 1;
  /* estimate the length we'll need */
  for(p = ucs; (c = *p); ++p) {
    switch(c) {
    default:
      if(c > 127 || c < 32) {
      case '"':
      case '&':
      case '<':
      case '>':
	n += 12;
	break;
      } else
	n++;
    }
  }
  /* format the string */
  b = bp = xmalloc_noptr(n);
  for(p = ucs; (c = *p); ++p) {
    switch(c) {
    default:
      if(*p > 127 || *p < 32) {
      case '"':
      case '&':
      case '<':
      case '>':
	bp += sprintf(bp, "&#%lu;", (unsigned long)c);
	break;
      } else
	*bp++ = c;
    }
  }
  *bp = 0;
  return b;
}

void cgi_attr(struct sink *output, const char *name, const char *value) {
  if(!value[strspn(value, "abcdefghijklmnopqrstuvwxyz"
		   "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		   "0123456789")])
    sink_printf(output, "%s=%s", name, value);
  else
    sink_printf(output, "%s=\"%s\"", name, cgi_sgmlquote(value, 0));
}

void cgi_opentag(struct sink *output, const char *name, ...) {
  va_list ap;
  const char *n, *v;
   
  sink_printf(output, "<%s", name);
  va_start(ap, name);
  while((n = va_arg(ap, const char *))) {
    sink_printf(output, " ");
    v = va_arg(ap, const char *);
    if(v)
      cgi_attr(output, n, v);
    else
      sink_printf(output, n);
  }
  sink_printf(output, ">");
}

void cgi_closetag(struct sink *output, const char *name) {
  sink_printf(output, "</%s>", name);
}

static int template_open(const char *name,
			 const char *ext,
			 const char **filenamep) {
  const char *dirs[2];
  int fd = -1, n;
  char *fullpath;

  dirs[0] = pkgconfdir;
  dirs[1] = pkgdatadir;
  if(name[0] == '/') {
    if((fd = open(name, O_RDONLY)) < 0) fatal(0, "cannot open %s", name);
    *filenamep = name;
  } else {
    for(n = 0; n < config->templates.n + (int)(sizeof dirs / sizeof *dirs); ++n) {
      byte_xasprintf(&fullpath, "%s/%s%s",
		     n < config->templates.n ? config->templates.s[n]
		                             : dirs[n - config->templates.n],
		     name, ext);
      if((fd = open(fullpath, O_RDONLY)) >= 0) break;
    }
    if(fd < 0) error(0, "cannot find %s%s in template path", name, ext);
    *filenamep = fullpath;
  }
  return fd;
}

static int valid_template_name(const char *name) {
  if(strchr(name, '/') || name[0] == '.')
    return 0;
  return 1;
}

void cgi_expand(const char *template,
		const struct cgi_expansion *expansions,
		size_t nexpansions,
		cgi_sink *output,
		void *u) {
  int fd = -1;
  int n;
  off_t m;
  char *b;
  struct stat sb;

  if(!valid_template_name(template))
    fatal(0, "invalid template name '%s'", template);
  if((fd = template_open(template, ".html", &template)) < 0)
    exitfn(EXIT_FAILURE);
  if(fstat(fd, &sb) < 0) fatal(errno, "cannot stat %s", template);
  m = 0;
  b = xmalloc_noptr(sb.st_size + 1);
  while(m < sb.st_size) {
    n = read(fd, b + m, sb.st_size - m);
    if(n > 0) m += n;
    else if(n == 0) fatal(0, "unexpected EOF reading %s", template);
    else if(errno != EINTR) fatal(errno, "error reading %s", template);
  }
  b[sb.st_size] = 0;
  xclose(fd);
  cgi_expand_string(template, b, expansions, nexpansions, output, u);
}

/** @brief Return a linked list of the parse of @p template */
static struct cgi_element *cgi_parse_string(const char *name,
					    const char *template) {
  int braces, line = 1, sline;
  const char *p;
  struct vector v;
  struct dynstr d;
  struct cgi_element *head = 0, **tailp = &head, *e;

  while(*template) {
    if(*template != '@') {
      sline = line;
      dynstr_init(&d);
      /* Gather up text without any expansions in. */
      while(*template && *template != '@') {
	if(*template == '\n')
	  ++line;
	dynstr_append(&d, *template++);
      }
      dynstr_terminate(&d);
      e = xmalloc(sizeof *e);
      e->next = 0;
      e->line = sline;
      e->type = ELEMENT_TEXT;
      e->text = d.vec;
      *tailp = e;
      tailp = &e->next;
      continue;
    }
    vector_init(&v);
    braces = 0;
    p = template;
    ++template;
    sline = line;
    while(*template != '@') {
      /* Skip whitespace */
      while(isspace((unsigned char)*template))
	++template;
      dynstr_init(&d);
      if(*template == '{') {
	/* bracketed arg */
	++template;
	while(*template && (*template != '}' || braces > 0)) {
	  switch(*template) {
	  case '{': ++braces; break;
	  case '}': --braces; break;
	  case '\n': ++line; break;
	  }
	  dynstr_append(&d, *template++);
	}
	if(!*template) fatal(0, "%s:%d: unterminated expansion '%.*s'",
			     name, sline, (int)(template - p), p);
	++template;
	if(isspace((unsigned char)*template)) {
	  /* We have @{...}<WHITESPACE><SOMETHING> */
	  for(p = template; isspace((unsigned char)*p); ++p)
	    ;
	  /* Now we are looking at <SOMETHING>.  If it's "{" then that
	   * must be the next argument.  Otherwise we infer that this
	   * is really the end of the expansion. */
	  if(*p != '{')
	    goto finished_expansion;
	}
      } else {
	/* unbracketed arg */
	while(*template
	      && *template != '@' && *template != '{' && *template != ':') {
	  if(*template == '\n') ++line;
	  dynstr_append(&d, *template++);
	}
	if(*template == ':')
	  ++template;
	if(!*template) fatal(0, "%s:%d: unterminated expansion '%.*s'",
			     name, sline, (int)(template - p), p);
	/* trailing whitespace is not significant in unquoted args */
	while(d.nvec && (isspace((unsigned char)d.vec[d.nvec - 1])))
	  --d.nvec;
      }
      dynstr_terminate(&d);
      vector_append(&v, d.vec);
    }
    ++template;
    finished_expansion:
    vector_terminate(&v);
    /* @@ terminates this file */
    if(v.nvec == 0)
      break;
    e = xmalloc(sizeof *e);
    e->next = 0;
    e->line = sline;
    e->type = ELEMENT_EXPANSION;
    e->name = v.vec[0];
    e->nargs = v.nvec - 1;
    e->args = &v.vec[1];
    *tailp = e;
    tailp = &e->next;
  }
  return head;
}

void cgi_expand_string(const char *name,
		       const char *template,
		       const struct cgi_expansion *expansions,
		       size_t nexpansions,
		       cgi_sink *output,
		       void *u) {
  int n, m;
  char *argname;
  struct dynstr d;
  cgi_sink parameter_output;
  const struct cgi_macro *macro;

  struct cgi_element *e;

  for(e = cgi_parse_string(name, template); e; e = e->next) {
    switch(e->type) {
    case ELEMENT_TEXT:
      output->sink->write(output->sink, e->text, strlen(e->text));
      break;
    case ELEMENT_EXPANSION:
      if((n = table_find(expansions,
			 offsetof(struct cgi_expansion, name),
			 sizeof (struct cgi_expansion),
			 nexpansions,
			 e->name)) >= 0) {
	/* We found a built-in */
	if(e->nargs < expansions[n].minargs)
	  fatal(0, "%s:%d: insufficient arguments to @%s@ (min %d, got %d)",
		name, e->line, e->name, expansions[n].minargs, e->nargs);
	if(e->nargs > expansions[n].maxargs)
	  fatal(0, "%s:%d: too many arguments to @%s@ (max %d, got %d)",
		name, e->line, e->name, expansions[n].maxargs, e->nargs);
	/* for ordinary expansions, recursively expand the arguments */
	if(!(expansions[n].flags & EXP_MAGIC)) {
	  for(m = 0; m < e->nargs; ++m) {
	    dynstr_init(&d);
	    byte_xasprintf(&argname, "<%s:%d arg #%d>", name, e->line, m);
	    parameter_output.quote = 0;
	    parameter_output.sink = sink_dynstr(&d);
	    cgi_expand_string(argname, e->args[m],
			      expansions, nexpansions,
			      &parameter_output, u);
	    dynstr_terminate(&d);
	    e->args[m] = d.vec;
	  }
	}
	expansions[n].handler(e->nargs, e->args, output, u);
      } else if(cgi_macros && (macro = hash_find(cgi_macros, e->name))) {
	/* We found a macro */
	if(e->nargs != macro->nargs)
	  fatal(0, "%s:%d: wrong number of arguments to @%s@ (need %d, got %d)",
		name, e->line, e->name, macro->nargs, e->nargs);
	/* We must substitute in argument values */
	/* TODO  */
	cgi_expand_string(e->name,
			  macro->value,
			  expansions,
			  nexpansions,
			  output,
			  u);
      } else {
	/* Totally undefined */
	fatal(0, "%s:%d: unknown expansion '%s'", name, e->line, e->name);
      }
      break;
    }
  }
}

char *cgi_makeurl(const char *url, ...) {
  va_list ap;
  struct kvp *kvp, *k, **kk = &kvp;
  struct dynstr d;
  const char *n, *v;
  
  dynstr_init(&d);
  dynstr_append_string(&d, url);
  va_start(ap, url);
  while((n = va_arg(ap, const char *))) {
    v = va_arg(ap, const char *);
    *kk = k = xmalloc(sizeof *k);
    kk = &k->next;
    k->name = n;
    k->value = v;
  }
  *kk = 0;
  if(kvp) {
    dynstr_append(&d, '?');
    dynstr_append_string(&d, kvp_urlencode(kvp, 0));
  }
  dynstr_terminate(&d);
  return d.vec;
}

void cgi_set_option(const char *name, const char *value) {
  struct kvp *k = xmalloc(sizeof *k);

  k->next = labels;
  k->name = name;
  k->value = value;
  labels = k;
}

static void option_label(int attribute((unused)) nvec,
			 char **vec) {
  cgi_set_option(vec[0], vec[1]);
}

static void option_include(int attribute((unused)) nvec,
			   char **vec) {
  include_options(vec[0]);
}

static void option_columns(int nvec,
			    char **vec) {
  struct column *c = xmalloc(sizeof *c);
  
  c->next = columns;
  c->name = vec[0];
  c->ncolumns = nvec - 1;
  c->columns = &vec[1];
  columns = c;
}

static struct option {
  const char *name;
  int minargs, maxargs;
  void (*handler)(int nvec, char **vec);
} options[] = {
  { "columns", 1, INT_MAX, option_columns },
  { "include", 1, 1, option_include },
  { "label", 2, 2, option_label },
};

struct read_options_state {
  const char *name;
  int line;
};

static void read_options_error(const char *msg,
			       void *u) {
  struct read_options_state *cs = u;
  
  error(0, "%s:%d: %s", cs->name, cs->line, msg);
}

static void include_options(const char *name) {
  int n, i;
  int fd;
  FILE *fp;
  char **vec, *buffer;
  struct read_options_state cs;

  if((fd = template_open(name, "", &cs.name)) < 0) return;
  if(!(fp = fdopen(fd, "r"))) fatal(errno, "error calling fdopen");
  cs.line = 0;
  while(!inputline(cs.name, fp, &buffer, '\n')) {
    ++cs.line;
    if(!(vec = split(buffer, &n, SPLIT_COMMENTS|SPLIT_QUOTES,
		     read_options_error, &cs)))
      continue;
    if(!n) continue;
    if((i = TABLE_FIND(options, struct option, name, vec[0])) == -1) {
      error(0, "%s:%d: unknown option '%s'", cs.name, cs.line, vec[0]);
      continue;
    }
    ++vec;
    --n;
    if(n < options[i].minargs) {
      error(0, "%s:%d: too few arguments to '%s'", cs.name, cs.line, vec[-1]);
      continue;
    }
    if(n > options[i].maxargs) {
      error(0, "%s:%d: too many arguments to '%s'", cs.name, cs.line, vec[-1]);
      continue;
    }
    options[i].handler(n, vec);
  }
  fclose(fp);
}

static void read_options(void) {
  if(!have_read_options) {
    have_read_options = 1;
    include_options("options");
  }
}

const char *cgi_label(const char *key) {
  const char *label;

  read_options();
  if(!(label = kvp_get(labels, key))) {
    /* No label found */
    if(!strncmp(key, "images.", 7)) {
      static const char *url_static;
      /* images.X defaults to <url.static>X.png */

      if(!url_static)
	url_static = cgi_label("url.static");
      byte_xasprintf((char **)&label, "%s%s.png", url_static, key + 7);
    } else if((label = strchr(key, '.')))
      /* X.Y defaults to Y */
      ++label;
    else
      /* otherwise default to label name */
      label = key;
  }
  return label;
}

int cgi_label_exists(const char *key) {
  read_options();
  return kvp_get(labels, key) ? 1 : 0;
}

char **cgi_columns(const char *name, int *ncolumns) {
  struct column *c;

  read_options();
  for(c = columns; c && strcmp(name, c->name); c = c->next)
    ;
  if(c) {
    if(ncolumns)
      *ncolumns = c->ncolumns;
    return c->columns;
  } else {
    if(ncolumns)
      *ncolumns = 0;
    return 0;
  }
}

void cgi_define(const char *name,
		int nargs,
		char **args,
		const char *value) {
  struct cgi_macro m;

  if(!cgi_macros)
    cgi_macros = hash_new(sizeof(struct cgi_macro));
  m.nargs = nargs;
  m.args = args;
  m.value = value;
  hash_add(cgi_macros, name, &m, HASH_INSERT_OR_REPLACE);
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
End:
*/
