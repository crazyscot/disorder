/*
 * This file is part of DisOrder
 * Copyright (C) 2005, 2007 Richard Kettlewell
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
/** @file lib/mime.c
 * @brief Support for MIME and allied protocols
 */

#include <config.h>
#include "types.h"

#include <string.h>
#include <ctype.h>

#include <stdio.h>

#include "mem.h"
#include "mime.h"
#include "vector.h"
#include "hex.h"
#include "log.h"
#include "base64.h"

/** @brief Match whitespace characters */
static int whitespace(int c) {
  switch(c) {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
    return 1;
  default:
    return 0;
  }
}

/** @brief Match RFC2045 tspecial characters */
static int tspecial(int c) {
  switch(c) {
  case '(':
  case ')':
  case '<':
  case '>':
  case '@':
  case ',':
  case ';':
  case ':':
  case '\\':
  case '"':
  case '/':
  case '[':
  case ']':
  case '?':
  case '=':
    return 1;
  default:
    return 0;
  }
}

/** @brief Match RFC2616 seprator characters */
static int http_separator(int c) {
  switch(c) {
  case '(':
  case ')':
  case '<':
  case '>':
  case '@':
  case ',':
  case ';':
  case ':':
  case '\\':
  case '"':
  case '/':
  case '[':
  case ']':
  case '?':
  case '=':
  case '{':
  case '}':
  case ' ':
  case '\t':
    return 1;
  default:
    return 0;
  }
}

/** @brief Match CRLF */
static int iscrlf(const char *ptr) {
  return ptr[0] == '\r' && ptr[1] == '\n';
}

/** @brief Skip whitespace
 * @param rfc822_comments If true, skip RFC822 nested comments
 */
static const char *skipwhite(const char *s, int rfc822_comments) {
  int c, depth;
  
  for(;;) {
    switch(c = *s) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
      ++s;
      break;
    case '(':
      if(!rfc822_comments)
	return s;
      ++s;
      depth = 1;
      while(*s && depth) {
	c = *s++;
	switch(c) {
	case '(': ++depth; break;
	case ')': --depth; break;
	case '\\':
	  if(!*s) return 0;
	  ++s;
	  break;
	}
      }
      if(depth) return 0;
      break;
    default:
      return s;
    }
  }
}

/** @brief Test for a word character
 * @param c Character to test
 * @param special tspecial() (MIME/RFC2405) or http_separator() (HTTP/RFC2616)
 * @return 1 if @p c is a word character, else 0
 */
static int iswordchar(int c, int (*special)(int)) {
  return !(c <= ' ' || c > '~' || special(c));
}

/** @brief Parse an RFC1521/RFC2616 word
 * @param s Pointer to start of word
 * @param valuep Where to store value
 * @param special tspecial() (MIME/RFC2405) or http_separator() (HTTP/RFC2616)
 * @return Pointer just after end of word or NULL if there's no word
 *
 * A word is a token or a quoted-string.
 */
static const char *parseword(const char *s, char **valuep,
			     int (*special)(int)) {
  struct dynstr value[1];
  int c;

  dynstr_init(value);
  if(*s == '"') {
    ++s;
    while((c = *s++) != '"') {
      switch(c) {
      case '\\':
	if(!(c = *s++)) return 0;
      default:
	dynstr_append(value, c);
	break;
      }
    }
    if(!c) return 0;
  } else {
    if(!iswordchar((unsigned char)*s, special))
      return NULL;
    dynstr_init(value);
    while(iswordchar((unsigned char)*s, special))
      dynstr_append(value, *s++);
  }
  dynstr_terminate(value);
  *valuep = value->vec;
  return s;
}

/** @brief Parse an RFC1521/RFC2616 token
 * @param s Pointer to start of token
 * @param valuep Where to store value
 * @param special tspecial() (MIME/RFC2405) or http_separator() (HTTP/RFC2616)
 * @return Pointer just after end of token or NULL if there's no token
 */
static const char *parsetoken(const char *s, char **valuep,
			      int (*special)(int)) {
  if(*s == '"') return 0;
  return parseword(s, valuep, special);
}

/** @brief Parse a MIME content-type field
 * @param s Start of field
 * @param typep Where to store type
 * @param parameternamep Where to store parameter name
 * @param parameternvaluep Wher to store parameter value
 * @return 0 on success, non-0 on error
 *
 * See <a href="http://tools.ietf.org/html/rfc2045#section-5">RFC 2045 s5</a>.
 */
int mime_content_type(const char *s,
		      char **typep,
		      char **parameternamep,
		      char **parametervaluep) {
  struct dynstr type, parametername;

  dynstr_init(&type);
  if(!(s = skipwhite(s, 1))) return -1;
  if(!*s) return -1;
  while(*s && !tspecial(*s) && !whitespace(*s))
    dynstr_append(&type, tolower((unsigned char)*s++));
  if(!(s = skipwhite(s, 1))) return -1;
  if(*s++ != '/') return -1;
  dynstr_append(&type, '/');
  if(!(s = skipwhite(s, 1))) return -1;
  while(*s && !tspecial(*s) && !whitespace(*s))
    dynstr_append(&type, tolower((unsigned char)*s++));
  if(!(s = skipwhite(s, 1))) return -1;

  if(*s == ';') {
    dynstr_init(&parametername);
    ++s;
    if(!(s = skipwhite(s, 1))) return -1;
    if(!*s) return -1;
    while(*s && !tspecial(*s) && !whitespace(*s))
      dynstr_append(&parametername, tolower((unsigned char)*s++));
    if(!(s = skipwhite(s, 1))) return -1;
    if(*s++ != '=') return -1;
    if(!(s = skipwhite(s, 1))) return -1;
    if(!(s = parseword(s, parametervaluep, tspecial))) return -1;
    if(!(s = skipwhite(s, 1))) return -1;
    dynstr_terminate(&parametername);
    *parameternamep = parametername.vec;
  } else
    *parametervaluep = *parameternamep = 0;
  dynstr_terminate(&type);
  *typep = type.vec;
  return 0;
}

/** @brief Parse a MIME message
 * @param s Start of message
 * @param callback Called for each header field
 * @param u Passed to callback
 * @return Pointer to decoded body (might be in original string), or NULL on error
 *
 * This does an RFC 822-style parse and honors Content-Transfer-Encoding as
 * described in <a href="http://tools.ietf.org/html/rfc2045#section-6">RFC 2045
 * s6</a>.  @p callback is called for each header field encountered, in order,
 * with ASCII characters in the header name forced to lower case.
 */
const char *mime_parse(const char *s,
		       int (*callback)(const char *name, const char *value,
				       void *u),
		       void *u) {
  struct dynstr name, value;
  char *cte = 0, *p;
  
  while(*s && !iscrlf(s)) {
    dynstr_init(&name);
    dynstr_init(&value);
    while(*s && !tspecial(*s) && !whitespace(*s))
      dynstr_append(&name, tolower((unsigned char)*s++));
    if(!(s = skipwhite(s, 1))) return 0;
    if(*s != ':') return 0;
    ++s;
    while(*s && !(*s == '\n' && !(s[1] == ' ' || s[1] == '\t')))
      dynstr_append(&value, *s++);
    if(*s) ++s;
    dynstr_terminate(&name);
    dynstr_terminate(&value);
    if(!strcmp(name.vec, "content-transfer-encoding")) {
      cte = xstrdup(value.vec);
      for(p = cte; *p; p++)
	*p = tolower((unsigned char)*p);
    }
    if(callback(name.vec, value.vec, u)) return 0;
  }
  if(*s) s += 2;
  if(cte) {
    if(!strcmp(cte, "base64")) return mime_base64(s, 0);
    if(!strcmp(cte, "quoted-printable")) return mime_qp(s);
  }
  return s;
}

/** @brief Match the boundary string */
static int isboundary(const char *ptr, const char *boundary, size_t bl) {
  return (ptr[0] == '-'
	  && ptr[1] == '-'
	  && !strncmp(ptr + 2, boundary, bl)
	  && (iscrlf(ptr + bl + 2)
	      || (ptr[bl + 2] == '-'
		  && ptr[bl + 3] == '-'
		  && (iscrlf(ptr + bl + 4) || *(ptr + bl + 4) == 0))));
}

/** @brief Match the final boundary string */
static int isfinal(const char *ptr, const char *boundary, size_t bl) {
  return (ptr[0] == '-'
	  && ptr[1] == '-'
	  && !strncmp(ptr + 2, boundary, bl)
	  && ptr[bl + 2] == '-'
	  && ptr[bl + 3] == '-'
	  && (iscrlf(ptr + bl + 4) || *(ptr + bl + 4) == 0));
}

/** @brief Parse a multipart MIME body
 * @param s Start of message
 * @param callback Callback for each part
 * @param boundary Boundary string
 * @param u Passed to callback
 * @return 0 on success, non-0 on error
 * 
 * See <a href="http://tools.ietf.org/html/rfc2046#section-5.1">RFC 2046
 * s5.1</a>.  @p callback is called for each part (not yet decoded in any way)
 * in succession; you should probably call mime_parse() for each part.
 */
int mime_multipart(const char *s,
		   int (*callback)(const char *s, void *u),
		   const char *boundary,
		   void *u) {
  size_t bl = strlen(boundary);
  const char *start, *e;
  int ret;

  /* We must start with a boundary string */
  if(!isboundary(s, boundary, bl))
    return -1;
  /* Keep going until we hit a final boundary */
  while(!isfinal(s, boundary, bl)) {
    s = strstr(s, "\r\n") + 2;
    start = s;
    while(!isboundary(s, boundary, bl)) {
      if(!(e = strstr(s, "\r\n")))
	return -1;
      s = e + 2;
    }
    if((ret = callback(xstrndup(start,
				s == start ? 0 : s - start - 2),
		       u)))
      return ret;
  }
  return 0;
}

/** @brief Parse an RFC2388-style content-disposition field
 * @param s Start of field
 * @param typep Where to store type
 * @param parameternamep Where to store parameter name
 * @param parameternvaluep Wher to store parameter value
 * @return 0 on success, non-0 on error
 *
 * See <a href="http://tools.ietf.org/html/rfc2388#section-3">RFC 2388 s3</a>
 * and <a href="http://tools.ietf.org/html/rfc2183">RFC 2183</a>.
 */
int mime_rfc2388_content_disposition(const char *s,
				     char **dispositionp,
				     char **parameternamep,
				     char **parametervaluep) {
  struct dynstr disposition, parametername;

  dynstr_init(&disposition);
  if(!(s = skipwhite(s, 1))) return -1;
  if(!*s) return -1;
  while(*s && !tspecial(*s) && !whitespace(*s))
    dynstr_append(&disposition, tolower((unsigned char)*s++));
  if(!(s = skipwhite(s, 1))) return -1;

  if(*s == ';') {
    dynstr_init(&parametername);
    ++s;
    if(!(s = skipwhite(s, 1))) return -1;
    if(!*s) return -1;
    while(*s && !tspecial(*s) && !whitespace(*s))
      dynstr_append(&parametername, tolower((unsigned char)*s++));
    if(!(s = skipwhite(s, 1))) return -1;
    if(*s++ != '=') return -1;
    if(!(s = skipwhite(s, 1))) return -1;
    if(!(s = parseword(s, parametervaluep, tspecial))) return -1;
    if(!(s = skipwhite(s, 1))) return -1;
    dynstr_terminate(&parametername);
    *parameternamep = parametername.vec;
  } else
    *parametervaluep = *parameternamep = 0;
  dynstr_terminate(&disposition);
  *dispositionp = disposition.vec;
  return 0;
}

/** @brief Convert MIME quoted-printable
 * @param s Quoted-printable data
 * @return Decoded data
 *
 * See <a href="http://tools.ietf.org/html/rfc2045#section-6.7">RFC 2045
 * s6.7</a>.
 */
char *mime_qp(const char *s) {
  struct dynstr d;
  int c, a, b;
  const char *t;

  dynstr_init(&d);
  while((c = *s++)) {
    switch(c) {
    case '=':
      if((a = unhexdigitq(s[0])) != -1
	 && (b = unhexdigitq(s[1])) != -1) {
	dynstr_append(&d, a * 16 + b);
	s += 2;
      } else {
	t = s;
	while(*t == ' ' || *t == '\t') ++t;
	if(iscrlf(t)) {
	  /* soft line break */
	  s = t + 2;
	} else
	  return 0;
      }
      break;
    case ' ':
    case '\t':
      t = s;
      while(*t == ' ' || *t == '\t') ++t;
      if(iscrlf(t))
	/* trailing space is always eliminated */
	s = t;
      else
	dynstr_append(&d, c);
      break;
    default:
      dynstr_append(&d, c);
      break;
    }
  }
  dynstr_terminate(&d);
  return d.vec;
}

/** @brief Parse a RFC2109 Cookie: header
 * @param s Header field value
 * @param cd Where to store result
 * @return 0 on success, non-0 on error
 */
int parse_cookie(const char *s,
		 struct cookiedata *cd) {
  char *n = 0, *v = 0;

  memset(cd, 0, sizeof *cd);
  s = skipwhite(s, 0);
  while(*s) {
    /* Skip separators */
    if(*s == ';' || *s == ',') {
      ++s;
      s = skipwhite(s, 0);
      continue;
    }
    if(!(s = parsetoken(s, &n, http_separator))) return -1;
    s = skipwhite(s, 0);
    if(*s++ != '=') return -1;
    s = skipwhite(s, 0);
    if(!(s = parseword(s, &v, http_separator))) return -1;
    if(n[0] == '$') {
      /* Some bit of meta-information */
      if(!strcmp(n, "$Version"))
	cd->version = v;
      else if(!strcmp(n, "$Path")) {
	if(cd->ncookies > 0 && cd->cookies[cd->ncookies-1].path == 0)
	  cd->cookies[cd->ncookies-1].path = v;
	else {
	  error(0, "redundant $Path in Cookie: header");
	  return -1;
	}
      } else if(!strcmp(n, "$Domain")) {
	if(cd->ncookies > 0 && cd->cookies[cd->ncookies-1].domain == 0)
	  cd->cookies[cd->ncookies-1].domain = v;
	else {
	  error(0, "redundant $Domain in Cookie: header");
	  return -1;
	}
      }
    } else {
      /* It's a new cookie */
      cd->cookies = xrealloc(cd->cookies,
			     (cd->ncookies + 1) * sizeof (struct cookie));
      cd->cookies[cd->ncookies].name = n;
      cd->cookies[cd->ncookies].value = v;
      cd->cookies[cd->ncookies].path = 0;
      cd->cookies[cd->ncookies].domain = 0;
      ++cd->ncookies;
    }
    s = skipwhite(s, 0);
    if(*s && (*s != ',' && *s != ';')) {
      error(0, "missing separator in Cookie: header");
      return -1;
    }
  }
  return 0;
}

/** @brief Find a named cookie
 * @param cd Parse cookie data
 * @param name Name of cookie
 * @return Cookie structure or NULL if not found
 */
const struct cookie *find_cookie(const struct cookiedata *cd,
				 const char *name) {
  int n;

  for(n = 0; n < cd->ncookies; ++n)
    if(!strcmp(cd->cookies[n].name, name))
      return &cd->cookies[n];
  return 0;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
End:
*/
