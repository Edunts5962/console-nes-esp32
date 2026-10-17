// game_catalog.cpp - voir game_catalog.h.
#include "game_catalog.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
static void *cat_alloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // PSRAM de preference
  return p ? p : malloc(n);
}
#else
static void *cat_alloc(size_t n) { return malloc(n); }
#endif

static CatalogEntry *s_cat = NULL;
static int16_t *s_view = NULL;     // indices des jeux filtres, dans l'ordre
static int s_cap = 0, s_count = 0, s_dropped = 0;
static int s_matching = 0, s_page = 0, s_pages = 1, s_first = 0, s_on_page = 0;
static char s_query[64] = "";

bool catalog_reset() {
  if (!s_cat) {
    s_cat = (CatalogEntry *)cat_alloc(sizeof(CatalogEntry) * MAX_GAMES_TOTAL);
    s_view = (int16_t *)cat_alloc(sizeof(int16_t) * MAX_GAMES_TOTAL);
    if (!s_cat || !s_view) {
      free(s_cat); free(s_view);
      s_cat = NULL; s_view = NULL;
      s_cap = 0;
      return false;
    }
    s_cap = MAX_GAMES_TOTAL;
  }
  s_count = 0;
  s_dropped = 0;
  s_matching = s_first = s_on_page = s_page = 0;
  s_pages = 1;
  return true;
}

bool catalog_add(const char *name, uint32_t size, uint8_t where) {
  if (!s_cat || s_count >= s_cap) { s_dropped++; return false; }
  CatalogEntry *e = &s_cat[s_count++];
  strncpy(e->name, name, sizeof(e->name) - 1);
  e->name[sizeof(e->name) - 1] = '\0';
  e->size = size;
  e->where = where;
  return true;
}

static int cat_cmp(const void *a, const void *b) {
  const CatalogEntry *x = (const CatalogEntry *)a, *y = (const CatalogEntry *)b;
  int c = strcasecmp(x->name, y->name);
  if (c) return c;
  return (int)x->where - (int)y->where;
}

void catalog_sort() {
  if (s_cat && s_count > 1) qsort(s_cat, (size_t)s_count, sizeof(CatalogEntry), cat_cmp);
}

int catalog_total() { return s_count; }
int catalog_capacity() { return s_cap; }
int catalog_dropped() { return s_dropped; }

static bool ci_contains(const char *hay, size_t hay_len, const char *needle, size_t n) {
  if (n > hay_len) return false;
  for (size_t s = 0; s + n <= hay_len; s++) {
    size_t i = 0;
    while (i < n && tolower((unsigned char)hay[s + i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

bool catalog_name_matches(const char *filename, const char *query) {
  if (!query || !query[0]) return true;
  size_t len = strlen(filename);
  if (len > 4 && strcasecmp(filename + len - 4, ".nes") == 0) len -= 4;
  const char *q = query;
  while (*q) {
    while (*q == ' ') q++;
    if (!*q) break;
    const char *e = q;
    while (*e && *e != ' ') e++;
    if (!ci_contains(filename, len, q, (size_t)(e - q))) return false;
    q = e;
  }
  return true;
}

void catalog_set_view(const char *query, int page, int per_page) {
  catalog_set_view_list(query, page, per_page, NULL, 0);
}

void catalog_set_view_list(const char *query, int page, int per_page, const int16_t *order, int n_order) {
  if (per_page < 1) per_page = 1;
  if (query != s_query) snprintf(s_query, sizeof(s_query), "%s", query ? query : "");
  s_matching = 0;
  if (!s_cat) s_count = 0;
  const int n = order ? n_order : s_count;
  for (int k = 0; k < n; k++) {
    const int i = order ? order[k] : k;
    if (i < 0 || i >= s_count) continue;
    if (catalog_name_matches(s_cat[i].name, s_query)) s_view[s_matching++] = (int16_t)i;
  }
  s_pages = s_matching > 0 ? (s_matching + per_page - 1) / per_page : 1;
  if (page >= s_pages) page = s_pages - 1;
  if (page < 0) page = 0;
  s_page = page;
  s_first = page * per_page;
  s_on_page = s_matching - s_first;
  if (s_on_page > per_page) s_on_page = per_page;
  if (s_on_page < 0) s_on_page = 0;
}

int catalog_view_matching() { return s_matching; }
int catalog_view_page() { return s_page; }
int catalog_view_pages() { return s_pages; }
int catalog_view_first() { return s_first; }
int catalog_view_count() { return s_on_page; }
const char *catalog_view_query() { return s_query; }

const CatalogEntry *catalog_view_entry(int i) {
  if (!s_cat || i < 0 || i >= s_on_page) return NULL;
  return &s_cat[s_view[s_first + i]];
}

// Voir game_catalog.h
const CatalogEntry *catalog_entry(int ci) {
  if (!s_cat || ci < 0 || ci >= s_count) return NULL;
  return &s_cat[ci];
}

int catalog_view_global(int i) {
  if (!s_cat || i < 0 || i >= s_on_page) return -1;
  return s_view[s_first + i];
}

int catalog_view_position_of(int ci) {
  if (!s_cat || ci < 0 || ci >= s_count) return -1;
  for (int k = 0; k < s_matching; k++)
    if (s_view[k] == ci) return k;
  return -1;
}

const CatalogEntry *catalog_view_at(int pos) {
  if (!s_cat || pos < 0 || pos >= s_matching) return NULL;
  return &s_cat[s_view[pos]];
}
