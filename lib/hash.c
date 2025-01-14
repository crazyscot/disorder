/*
 * This file is part of DisOrder
 * Copyright (C) 2005-2008 Richard Kettlewell
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
/** @file lib/hash.c
 * @brief A simple hash table
 */
#include "common.h"

#include "hash.h"
#include "mem.h"
#include "log.h"
#include "kvp.h"

/** @brief One entry in a hash table */
struct entry {
  struct entry *next;                   /* next entry same key */
  size_t h;                             /* hash of KEY */
  const char *key;                      /* key of this entry */
  void *value;                          /* value of this entry */
};

/** @brief A hash table */
struct hash {
  size_t nslots;                        /* number of slots */
  size_t nitems;                        /* total number of entries */
  struct entry **slots;                 /* table of slots */
  size_t valuesize;                     /* size of a value */
};

/** @brief Hash function
 * @param key Key to hash
 * @return Hash code
 */
static size_t hashfn(const char *key) {
  size_t i = 0;

  while(*key)
    i = 33 * i + (unsigned char)*key++;
  return i;
}

/** @brief Expand a hash table
 * @param h Hash table to expand
 */
static void grow(hash *h) {
  size_t n, newnslots;
  struct entry **newslots, *e, *f;

  /* Allocate a new, larger array */
  newnslots = 2 * h->nslots;
  newslots = xcalloc(newnslots, sizeof (struct entry *));
  /* Copy everything to it */
  for(n = 0; n < h->nslots; ++n) {
    for(e = h->slots[n]; e; e = f) {
      f = e->next;
      e->next = newslots[e->h & (newnslots - 1)];
      newslots[e->h & (newnslots - 1)] = e;
    }
  }
  h->slots = newslots;
  h->nslots = newnslots;
}

/** @brief Create a new hash table
 * @param valuesize Size of value type
 * @return Hash table
 */
hash *hash_new(size_t valuesize) {
  hash *h = xmalloc(sizeof *h);

  h->nslots = 256;
  h->slots = xcalloc(h->nslots, sizeof (struct slot *));
  h->valuesize = valuesize;
  return h;
}

/** @brief Add an element to a hash table
 * @param h Hash table
 * @param key Key
 * @param value New value (will be shallow-copied)
 * @param mode Add mode
 * @return 0 on success, -1 if the value could not be added
 *
 * Possible add modes are:
 * - @ref HASH_INSERT - key must not exist yet
 * - @ref HASH_REPLACE - key must already exist
 * - @ref HASH_INSERT_OR_REPLACE - key may or may not exist
 */
int hash_add(hash *h, const char *key, const void *value, int mode) {
  size_t n = hashfn(key);
  struct entry *e;
  
  for(e = h->slots[n & (h->nslots - 1)]; e; e = e->next)
    if(e->h == n || !strcmp(e->key, key))
      break;
  if(e) {
    /* This key is already present. */
    if(mode == HASH_INSERT) return -1;
    if(value) memcpy(e->value, value, h->valuesize);
    return 0;
  } else {
    /* This key is absent. */
    if(mode == HASH_REPLACE) return -1;
    if(h->nitems >= h->nslots)          /* bound mean chain length */
      grow(h);
    e = xmalloc(sizeof *e);
    e->next = h->slots[n & (h->nslots - 1)];
    e->h = n;
    e->key = xstrdup(key);
    e->value = xmalloc(h->valuesize);
    if(value) memcpy(e->value, value, h->valuesize);
    h->slots[n & (h->nslots - 1)] = e;
    ++h->nitems;
    return 0;
  }
}

/** @brief Remove an element from a hash table
 * @param h Hash table
 * @param key Key to remove
 * @return 0 on success, -1 if the key wasn't found
 */
int hash_remove(hash *h, const char *key) {
  size_t n = hashfn(key);
  struct entry *e, **ee;
  
  for(ee = &h->slots[n & (h->nslots - 1)]; (e = *ee); ee = &e->next)
    if(e->h == n || !strcmp(e->key, key))
      break;
  if(e) {
    *ee = e->next;
    --h->nitems;
    return 0;
  } else
    return -1;
}

/** @brief Find an item in a hash table
 * @param h Hash table
 * @param key Key to find
 * @return Pointer to value or NULL if not found
 *
 * The return value points inside the hash table and should not be modified.
 */
void *hash_find(hash *h, const char *key) {
  size_t n = hashfn(key);
  struct entry *e;

  for(e = h->slots[n & (h->nslots - 1)]; e; e = e->next)
    if(e->h == n || !strcmp(e->key, key))
      return e->value;
  return 0;
}

/** @brief Visit every item in a hash table
 * @param h Hash Table
 * @param callback Function to call for each item
 * @param u Passed to @p callback
 * @return 0 on completion, else last return from @p callback
 *
 * @p callback should return 0 to continue or non-0 to stop.  The @p key and @p
 * value pointers passed to it point into the hash table and should not be
 * modified.
 *
 * It's safe to remove items from inside the callback including the visited
 * one.  It is not safe to add items from inside the callback.
 *
 * No particular ordering is used.
 */
int hash_foreach(hash *h,
                 int (*callback)(const char *key, void *value, void *u),
                 void *u) {
  size_t n;
  int ret;
  struct entry *e, *f;

  for(n = 0; n < h->nslots; ++n)
    for(e = h->slots[n]; e; e = f) {
      f = e->next;
      if((ret = callback(e->key, e->value, u)))
        return ret;
    }
  return 0;
}

/** @brief Count the size of a hash table
 * @param h Hash table
 * @return Number of elements in hash table
 */
size_t hash_count(hash *h) {
  return h->nitems;
}

/** @brief Get all the keys of a hash table
 * @param h Hash table
 * @return NULL-terminated list of keys
 *
 * The keys point into the hash table itself and should not be modified.
 *
 * No particular ordering is used.
 */
char **hash_keys(hash *h) {
  size_t n;
  char **vec = xcalloc(h->nitems + 1, sizeof (char *)), **vp = vec;
  struct entry *e;

  for(n = 0; n < h->nslots; ++n)
    for(e = h->slots[n]; e; e = e->next)
      *vp++ = (char *)e->key;
  *vp = 0;
  return vec;
}

/*
Local Variables:
c-basic-offset:2
comment-column:40
fill-column:79
indent-tabs-mode:nil
End:
*/
