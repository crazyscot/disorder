/*
 * This file is part of DisOrder.
 * Copyright (C) 2005, 2007, 2008 Richard Kettlewell
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
#include "test.h"

static void test_regsub(void) {
  pcre *re;
  const char *errstr;
  int erroffset;

  check_integer(regsub_flags(""), 0);
  check_integer(regsub_flags("g"), REGSUB_GLOBAL);
  check_integer(regsub_flags("i"), REGSUB_CASE_INDEPENDENT);
  check_integer(regsub_flags("gi"), REGSUB_GLOBAL|REGSUB_CASE_INDEPENDENT);
  check_integer(regsub_flags("iiggxx"), REGSUB_GLOBAL|REGSUB_CASE_INDEPENDENT);
  check_integer(regsub_compile_options(0), 0);
  check_integer(regsub_compile_options(REGSUB_CASE_INDEPENDENT), PCRE_CASELESS);
  check_integer(regsub_compile_options(REGSUB_GLOBAL|REGSUB_CASE_INDEPENDENT), PCRE_CASELESS);
  check_integer(regsub_compile_options(REGSUB_GLOBAL), 0);

  re = pcre_compile("foo", PCRE_UTF8, &errstr, &erroffset, 0);
  assert(re != 0);
  check_string(regsub(re, "wibble-foo-foo-bar", "spong", 0),
               "wibble-spong-foo-bar");
  check_string(regsub(re, "wibble-foo-foo-bar", "spong", REGSUB_GLOBAL),
               "wibble-spong-spong-bar");
  check_string(regsub(re, "wibble-x-x-bar", "spong", REGSUB_GLOBAL),
               "wibble-x-x-bar");
  insist(regsub(re, "wibble-x-x-bar", "spong", REGSUB_MUST_MATCH) == 0);

  re = pcre_compile("a+", PCRE_UTF8, &errstr, &erroffset, 0);
  assert(re != 0);
  check_string(regsub(re, "baaaaa", "spong", 0),
               "bspong");
  check_string(regsub(re, "baaaaa", "spong", REGSUB_GLOBAL),
               "bspong");
  check_string(regsub(re, "baaaaa", "foo-$&-bar", 0),
               "bfoo-aaaaa-bar");

  re = pcre_compile("(a+)(b+)", PCRE_UTF8|PCRE_CASELESS, &errstr, &erroffset, 0);
  assert(re != 0);
  check_string(regsub(re, "foo-aaaabbb-bar", "spong", 0),
               "foo-spong-bar");
  check_string(regsub(re, "foo-aaaabbb-bar", "x:$2/$1:y", 0),
               "foo-x:bbb/aaaa:y-bar");
  check_string(regsub(re, "foo-aAaAbBb-bar", "x:$2$$$1:y", 0),
               "foo-x:bBb$aAaA:y-bar");
}

TEST(regsub);

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/