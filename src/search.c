/*
 * BFpilot - indexed filename/path search.
 */

#include "search.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "diag.h"
#include "paths.h"
#include "websrv.h"

#define BFPILOT_SEARCH_ALL_ROOTS_LABEL "all"
#define BFPILOT_SEARCH_SYSTEM_ROOT_LABEL "system"
#define BFPILOT_SEARCH_DEFAULT_ROOT BFPILOT_SEARCH_ALL_ROOTS_LABEL
#define BFPILOT_SEARCH_MAX_ENTRIES 2000000UL
#define BFPILOT_SEARCH_MAX_ROOTS 64
#define BFPILOT_SEARCH_MAX_QUERY 256
#define BFPILOT_SEARCH_MAX_TERMS 8
#define BFPILOT_SEARCH_DEFAULT_LIMIT 200UL
#define BFPILOT_SEARCH_MAX_LIMIT 1000UL
#define BFPILOT_SEARCH_PROGRESS_INTERVAL 2048UL
#define BFPILOT_SEARCH_CRAWL_NICE 4
/* Single-threaded crawl: multi-worker + shared visited/index was a crash/hang
 * risk under P2JB (lock nesting + FS thrash). Sequential is still fast for
 * typical PS5 path counts and far more stable. */
#define BFPILOT_SEARCH_ROOT_ERR_ABORT 64

typedef struct json_buf {
  char  *data;
  size_t len;
  size_t cap;
} json_buf_t;

typedef struct search_entry {
  char               *path;
  size_t              name_offset;
  int                 dir;
  unsigned long long  size;
  long                mtime;
} search_entry_t;

typedef struct string_arena {
  char *ptr;
  size_t used;
  size_t cap;
  struct string_arena *next;
} string_arena_t;

typedef struct search_index {
  search_entry_t    *entries;
  char              *string_block;
  string_arena_t    *arena;
  size_t             count;
  size_t             cap;
  char               root[1024];
  time_t             built_at;
  long               build_ms;
  unsigned long      dirs_scanned;
  unsigned long      files_indexed;
  unsigned long      dirs_indexed;
  unsigned long      skipped;
  unsigned long      errors;
  unsigned long long memory_estimate;
  dev_t              root_dev;
  int                truncated;
} search_index_t;


typedef struct search_state {
  pthread_mutex_t    lock;
  search_index_t    *index;
  int                running;
  int                cancel;
  int                stale;
  int                truncated;
  int                last_errno;
  unsigned long      stale_generation;
  unsigned long      build_generation;
  char               root[1024];
  char               current[1024];
  char               error[256];
  char               stale_reason[96];
  char               last_query[128];
  char               roots_ok[768];
  char               roots_fail[512];
  char               roots_skip[512];
  int                roots_tried;
  int                roots_succeeded;
  int                roots_failed;
  time_t             started_at;
  time_t             ended_at;
  long               elapsed_ms;
  unsigned long      entries_seen;
  unsigned long      dirs_scanned;
  unsigned long      files_indexed;
  unsigned long      dirs_indexed;
  unsigned long      skipped;
  unsigned long      errors;
  unsigned long      mount_skips;
  unsigned long      last_query_scanned;
  unsigned long      last_query_matched;
  unsigned long      last_query_returned;
  long               last_query_ms;
  unsigned long long memory_estimate;
} search_state_t;

typedef struct crawler_arg {
  char          root[1024];
  int           global;
  int           system;
  unsigned long start_stale_generation;
} crawler_arg_t;

static search_state_t g_search = {
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .stale = 1,
  .stale_reason = "not indexed",
};


static long
search_monotonic_ms(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}


static void
search_diag_log(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof(line), fmt ? fmt : "", ap);
  va_end(ap);
  if(n <= 0) return;
  if((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
  if(n > 0 && line[n-1] != '\n' && (size_t)n < sizeof(line) - 1) {
    line[n++] = '\n';
    line[n] = 0;
  }
  FILE *file = fopen(BFPILOT_SEARCH_CRAWL_LOG, "a");
  if(file) {
    fwrite(line, 1, (size_t)n, file);
    fclose(file);
  }
}


static int
json_grow(json_buf_t *b, size_t add) {
  if(b->len + add + 1 <= b->cap) return 0;
  size_t next = b->cap ? b->cap : 1024;
  while(next < b->len + add + 1) {
    if(next > SIZE_MAX / 2) return -1;
    next *= 2;
  }
  char *p = realloc(b->data, next);
  if(!p) return -1;
  b->data = p;
  b->cap = next;
  return 0;
}


static int
json_append(json_buf_t *b, const char *s) {
  size_t n = strlen(s);
  if(json_grow(b, n) != 0) return -1;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = 0;
  return 0;
}


static int
json_appendf(json_buf_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list cp;
  va_copy(cp, ap);
  int n = vsnprintf(NULL, 0, fmt, cp);
  va_end(cp);
  if(n < 0) {
    va_end(ap);
    return -1;
  }
  if(json_grow(b, (size_t)n) != 0) {
    va_end(ap);
    return -1;
  }
  vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
  return 0;
}


static int
json_string(json_buf_t *b, const char *s) {
  if(json_append(b, "\"") != 0) return -1;
  for(const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch(*p) {
    case '\\': if(json_append(b, "\\\\") != 0) return -1; break;
    case '"':  if(json_append(b, "\\\"") != 0) return -1; break;
    case '\b': if(json_append(b, "\\b") != 0) return -1; break;
    case '\f': if(json_append(b, "\\f") != 0) return -1; break;
    case '\n': if(json_append(b, "\\n") != 0) return -1; break;
    case '\r': if(json_append(b, "\\r") != 0) return -1; break;
    case '\t': if(json_append(b, "\\t") != 0) return -1; break;
    default:
      if(*p < 0x20) {
        if(json_appendf(b, "\\u%04x", *p) != 0) return -1;
      } else {
        if(json_grow(b, 1) != 0) return -1;
        b->data[b->len++] = (char)*p;
        b->data[b->len] = 0;
      }
      break;
    }
  }
  return json_append(b, "\"");
}


static int
serve_owned(const http_request_t *req, int status, char *data, size_t size) {
  int rc = websrv_send(req->fd, status, "application/json", data, size);
  free(data);
  return rc;
}


static int
serve_error(const http_request_t *req, int status, const char *msg) {
  json_buf_t b = {0};
  if(json_append(&b, "{\"ok\":false,\"error\":") != 0 ||
     json_string(&b, msg ? msg : "error") != 0 ||
     json_append(&b, "}") != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, status, b.data, b.len);
}


static char *
search_strdup(const char *s) {
  size_t n = strlen(s ? s : "");
  char *out = malloc(n + 1);
  if(!out) return NULL;
  memcpy(out, s ? s : "", n + 1);
  return out;
}




static char *
search_arena_alloc(string_arena_t **arena, size_t size) {
  string_arena_t *a = *arena;
  if(!a || a->used + size > a->cap) {
    size_t cap = 16 * 1024 * 1024;
    if(size > cap) cap = size;
    string_arena_t *new_a = malloc(sizeof(*new_a));
    if(!new_a) return NULL;
    new_a->ptr = malloc(cap);
    if(!new_a->ptr) {
      free(new_a);
      return NULL;
    }
    new_a->used = 0;
    new_a->cap = cap;
    new_a->next = a;
    *arena = new_a;
    a = new_a;
  }
  char *res = a->ptr + a->used;
  a->used += size;
  return res;
}

static char *
search_arena_dup(string_arena_t **arena, const char *s) {
  size_t n = strlen(s ? s : "");
  char *out = search_arena_alloc(arena, n + 1);
  if(!out) return NULL;
  memcpy(out, s ? s : "", n + 1);
  return out;
}




static const char *
search_basename(const char *path) {
  const char *base = strrchr(path ? path : "", '/');
  if(base && base[1]) return base + 1;
  return path && path[0] ? path : "/";
}


static size_t
search_name_offset(const char *path) {
  const char *name = search_basename(path);
  return (size_t)(name - path);
}


static int
path_has_dotdot_segment(const char *path) {
  const char *p = path ? path : "";
  while(*p) {
    while(*p == '/') p++;
    const char *start = p;
    while(*p && *p != '/') p++;
    if((size_t)(p - start) == 2 && start[0] == '.' && start[1] == '.') {
      return 1;
    }
  }
  return 0;
}


static int
path_starts_with_root(const char *path, const char *root) {
  size_t n = strlen(root);
  return strncmp(path, root, n) == 0 && (path[n] == 0 || path[n] == '/');
}


static int
search_global_root_label(const char *root) {
  return !root || !root[0] ||
         !strcmp(root, BFPILOT_SEARCH_ALL_ROOTS_LABEL) ||
         !strcmp(root, "storage") || !strcmp(root, "*");
}


static int
search_system_root_label(const char *root) {
  return !strcmp(root ? root : "", BFPILOT_SEARCH_SYSTEM_ROOT_LABEL) ||
         !strcmp(root ? root : "", "root") ||
         !strcmp(root ? root : "", "everything") ||
         !strcmp(root ? root : "", "/");
}


static int
search_wide_path_allowed(const char *path) {
  return path && path[0] == '/' && !path_has_dotdot_segment(path);
}


static int
search_skip_system_path(const char *path) {
  /* Never descend into these trees during any crawl. */
  static const char *skip[] = {
    "/dev",
    "/net",
    "/proc",
    "/run",
    "/sys",
    "/mnt/sandbox",
    "/mnt/shadowmnt",
    "/system_tmp",
    NULL
  };
  for(int i = 0; skip[i]; i++) {
    if(path_starts_with_root(path, skip[i])) return 1;
  }
  return 0;
}


/* Roots we never select as crawl roots (still may exist as mount points). */
static int
search_reject_as_root(const char *path) {
  static const char *reject[] = {
    "/dev",
    "/net",
    "/proc",
    "/run",
    "/sys",
    "/mnt",           /* tmpfs hub; use /mnt/usb* /mnt/ext* only */
    "/mnt/sandbox",
    "/mnt/shadowmnt",
    "/system_tmp",    /* volatile tmpfs */
    "/update",        /* update package mount; low value, higher risk */
    NULL
  };
  if(!path || !path[0]) return 1;
  for(int i = 0; reject[i]; i++) {
    if(!strcmp(path, reject[i])) return 1;
  }
  return search_skip_system_path(path);
}


static void
append_root_csv(char *buf, size_t buf_sz, const char *item) {
  if(!buf || !buf_sz || !item || !item[0]) return;
  size_t used = strlen(buf);
  size_t need = strlen(item) + (used ? 1 : 0);
  if(used + need + 1 >= buf_sz) return;
  if(used) {
    buf[used++] = ',';
    buf[used] = 0;
  }
  snprintf(buf + used, buf_sz - used, "%s", item);
}


static void
add_system_root(char roots[][1024], int *count, const char *root,
                int have_base_dev, dev_t base_dev, int force) {
  if(*count >= BFPILOT_SEARCH_MAX_ROOTS ||
     !search_wide_path_allowed(root) ||
     search_reject_as_root(root)) {
    search_diag_log("ROOT SKIP (rule/full): %s", root);
    return;
  }
  struct stat st;
  if(lstat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
    search_diag_log("ROOT SKIP (missing/not dir): %s", root);
    return;
  }
  /* Same device as / is already covered by the / crawl with XDEV fencing,
   * unless force=1 for high-value nullfs-style views that must be explicit. */
  if(!force && strcmp(root, "/") && have_base_dev && st.st_dev == base_dev) {
    search_diag_log("ROOT SKIP (matches base_dev): %s", root);
    return;
  }
  for(int i = 0; i < *count; i++) {
    if(!strcmp(roots[i], root)) return;
  }
  /* Keep crawl log light — verbose ROOT ADD spam hurt FS under P2JB. */
  snprintf(roots[*count], 1024, "%s", root);
  (*count)++;
}


static int
collect_system_roots(char roots[][1024]) {
  int count = 0;
  struct stat root_st;
  int have_base_dev = lstat("/", &root_st) == 0 && S_ISDIR(root_st.st_mode);
  dev_t base_dev = have_base_dev ? root_st.st_dev : 0;

  /* Mount-level roots only. Nested force-roots re-walk the same trees and
   * thrash FS under P2JB. XDEV covers children within each mount. */
  static const char *priority[] = {
    "/data",
    "/user",
    NULL
  };
  for(int i = 0; priority[i]; i++) {
    add_system_root(roots, &count, priority[i], have_base_dev, base_dev, 1);
  }

  for(int i = 0; i < 8; i++) {
    char root[32];
    snprintf(root, sizeof(root), "/mnt/usb%d", i);
    add_system_root(roots, &count, root, 0, 0, 1);
    snprintf(root, sizeof(root), "/mnt/ext%d", i);
    add_system_root(roots, &count, root, 0, 0, 1);
  }

  static const char *system_candidates[] = {
    "/system",
    "/system_data",
    "/system_ex",
    "/preinst",
    "/preinst2",
    "/hostapp",
    "/download",
    "/usb",
    "/app",
    "/mnt/disc",
    NULL
  };
  for(int i = 0; system_candidates[i]; i++) {
    add_system_root(roots, &count, system_candidates[i], have_base_dev, base_dev, 0);
  }

  add_system_root(roots, &count, "/", 0, 0, 1);

  DIR *dir = opendir("/");
  if(dir) {
    struct dirent *ent;
    while((ent = readdir(dir)) != NULL) {
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(ent->d_type == DT_REG) continue;
      char child[1024];
      if(strlen(ent->d_name) + 2 >= sizeof(child)) continue;
      snprintf(child, sizeof(child), "/%s", ent->d_name);
      add_system_root(roots, &count, child, have_base_dev, base_dev, 0);
    }
    closedir(dir);
  }

  search_diag_log("ROOT COLLECT done count=%d", count);
  return count;
}


static int
collect_global_roots(char roots[][1024]) {
  return collect_system_roots(roots);
}


static void
join_path(char *out, size_t out_sz, const char *dir, const char *name) {
  size_t n = strlen(dir);
  snprintf(out, out_sz, "%s%s%s", dir, (n > 1 && dir[n - 1] != '/') ? "/" : "",
           name);
}


static void
search_index_free(search_index_t *idx) {
  if(!idx) return;
  if(idx->string_block) {
    free(idx->string_block);
  } else if (idx->arena) {
    string_arena_t *a = idx->arena;
    while(a) {
      string_arena_t *next = a->next;
      free(a->ptr);
      free(a);
      a = next;
    }
  } else {
    for(size_t i = 0; i < idx->count; i++) {
      free(idx->entries[i].path);
    }
  }
  free(idx->entries);
  free(idx);
}


static int
search_index_reserve(search_index_t *idx, size_t want) {
  if(want <= idx->cap) return 0;
  size_t next = idx->cap ? idx->cap : 1024;
  while(next < want) {
    if(next > SIZE_MAX / 2) return -1;
    next *= 2;
  }
  search_entry_t *entries = realloc(idx->entries, next * sizeof(*entries));
  if(!entries) return -1;
  idx->entries = entries;
  idx->cap = next;
  return 0;
}


static int
search_index_add(search_index_t *idx, const char *path, const struct stat *st) {
  if(idx->count >= BFPILOT_SEARCH_MAX_ENTRIES) {
    idx->truncated = 1;
    idx->skipped++;
    return 1;
  }
  if(search_index_reserve(idx, idx->count + 1) != 0) return -1;

  char *path_copy = search_arena_dup(&idx->arena, path);
  if(!path_copy) {
    return -1;
  }

  search_entry_t *entry = &idx->entries[idx->count++];
  entry->path = path_copy;
  entry->name_offset = search_name_offset(path_copy);
  entry->dir = S_ISDIR(st->st_mode) ? 1 : 0;
  entry->size = st->st_size > 0 ? (unsigned long long)st->st_size : 0;
  entry->mtime = (long)st->st_mtime;

  if(entry->dir) idx->dirs_indexed++;
  else idx->files_indexed++;
  idx->memory_estimate += sizeof(*entry) + strlen(path_copy) + 1;
  return 0;
}



static int
search_cancelled(void) {
  int cancel;
  if(websrv_exit_requested()) return 1;
  pthread_mutex_lock(&g_search.lock);
  cancel = g_search.cancel;
  pthread_mutex_unlock(&g_search.lock);
  return cancel;
}


#pragma pack(push, 1)
typedef struct {
  char magic[12];
  uint32_t version;
  uint64_t count;
  int64_t built_at;
  int64_t build_ms;
  uint64_t dirs_scanned;
  uint64_t files_indexed;
  uint64_t dirs_indexed;
  uint64_t skipped;
  uint64_t errors;
  uint64_t memory_estimate;
  char root[1024];
  uint64_t string_block_size;
} idx_header_t;

typedef struct {
  uint32_t path_offset;
  uint32_t name_offset;
  int32_t dir;
  uint64_t size;
  int64_t mtime;
} idx_entry_t;
#pragma pack(pop)

static void
search_save_index(search_index_t *idx) {
  if(!idx || idx->count == 0) return;
  FILE *f = fopen(BFPILOT_SEARCH_IDX_TMP, "wb");
  if(!f) return;

  uint64_t string_block_size = 0;
  for(size_t i=0; i<idx->count; i++) {
    string_block_size += strlen(idx->entries[i].path) + 1;
  }

  idx_header_t hdr = {0};
  snprintf(hdr.magic, sizeof(hdr.magic), "BFPILOT_IDX");
  hdr.version = 2;
  hdr.count = idx->count;
  hdr.built_at = idx->built_at;
  hdr.build_ms = idx->build_ms;
  hdr.dirs_scanned = idx->dirs_scanned;
  hdr.files_indexed = idx->files_indexed;
  hdr.dirs_indexed = idx->dirs_indexed;
  hdr.skipped = idx->skipped;
  hdr.errors = idx->errors;
  hdr.memory_estimate = idx->memory_estimate;
  snprintf(hdr.root, sizeof(hdr.root), "%s", idx->root);
  hdr.string_block_size = string_block_size;

  int ok = 1;
  if(fwrite(&hdr, sizeof(hdr), 1, f) != 1) ok = 0;

  idx_entry_t *disk_entries = malloc(idx->count * sizeof(idx_entry_t));
  if(disk_entries) {
    uint32_t current_offset = 0;
    for(size_t i=0; i<idx->count; i++) {
      disk_entries[i].path_offset = current_offset;
      current_offset += strlen(idx->entries[i].path) + 1;
      disk_entries[i].name_offset = (uint32_t)idx->entries[i].name_offset;
      disk_entries[i].dir = idx->entries[i].dir;
      disk_entries[i].size = idx->entries[i].size;
      disk_entries[i].mtime = idx->entries[i].mtime;
    }
    if(ok && fwrite(disk_entries, sizeof(idx_entry_t), idx->count, f) != idx->count) ok = 0;
    free(disk_entries);
  } else {
    ok = 0;
  }

  if(ok) {
    for(size_t i=0; i<idx->count; i++) {
      size_t len = strlen(idx->entries[i].path) + 1;
      if(fwrite(idx->entries[i].path, 1, len, f) != len) {
        ok = 0;
        break;
      }
    }
  }

  fflush(f);
  int fd = fileno(f);
  if(fd >= 0) fsync(fd);
  fclose(f);

  if(ok) {
    rename(BFPILOT_SEARCH_IDX_TMP, BFPILOT_SEARCH_IDX);
  } else {
    unlink(BFPILOT_SEARCH_IDX_TMP);
  }
}

static search_index_t *
search_load_index(void) {
  FILE *f = fopen(BFPILOT_SEARCH_IDX, "rb");
  if(!f) return NULL;

  idx_header_t hdr;
  if(fread(&hdr, sizeof(hdr), 1, f) != 1) {
    fclose(f);
    return NULL;
  }
  if(strcmp(hdr.magic, "BFPILOT_IDX") != 0 || hdr.version != 2) {
    fclose(f);
    return NULL;
  }
  /* Reject corrupt headers that would hang/OOM the HTTP thread. */
  if(hdr.count == 0 ||
     hdr.count > BFPILOT_SEARCH_MAX_ENTRIES ||
     hdr.string_block_size == 0 ||
     hdr.string_block_size > (uint64_t)BFPILOT_SEARCH_MAX_ENTRIES * 1024ULL) {
    search_diag_log("load_index reject count=%llu str=%llu",
                    (unsigned long long)hdr.count,
                    (unsigned long long)hdr.string_block_size);
    fclose(f);
    return NULL;
  }

  search_index_t *idx = calloc(1, sizeof(*idx));
  if(!idx) {
    fclose(f);
    return NULL;
  }

  idx->entries = malloc((size_t)hdr.count * sizeof(search_entry_t));
  idx->string_block = malloc((size_t)hdr.string_block_size);
  idx_entry_t *disk_entries = malloc((size_t)hdr.count * sizeof(idx_entry_t));

  if(!idx->entries || !idx->string_block || !disk_entries) {
    free(idx->entries);
    free(idx->string_block);
    free(disk_entries);
    free(idx);
    fclose(f);
    return NULL;
  }

  if(fread(disk_entries, sizeof(idx_entry_t), hdr.count, f) != hdr.count) goto fail;
  if(fread(idx->string_block, 1, hdr.string_block_size, f) != hdr.string_block_size) goto fail;

  for(size_t i=0; i<hdr.count; i++) {
    idx->entries[i].path = idx->string_block + disk_entries[i].path_offset;
    idx->entries[i].name_offset = disk_entries[i].name_offset;
    idx->entries[i].dir = disk_entries[i].dir;
    idx->entries[i].size = disk_entries[i].size;
    idx->entries[i].mtime = disk_entries[i].mtime;
  }

  idx->count = hdr.count;
  idx->cap = hdr.count;
  idx->built_at = hdr.built_at;
  idx->build_ms = hdr.build_ms;
  idx->dirs_scanned = hdr.dirs_scanned;
  idx->files_indexed = hdr.files_indexed;
  idx->dirs_indexed = hdr.dirs_indexed;
  idx->skipped = hdr.skipped;
  idx->errors = hdr.errors;
  idx->memory_estimate = hdr.memory_estimate;
  snprintf(idx->root, sizeof(idx->root), "%s", hdr.root);

  free(disk_entries);
  fclose(f);
  return idx;

fail:
  free(idx->entries);
  free(idx->string_block);
  free(disk_entries);
  free(idx);
  fclose(f);
  return NULL;
}

static void
publish_progress(const search_index_t *idx, const char *current,
                 const char *error, int last_errno) {
  pthread_mutex_lock(&g_search.lock);
  g_search.entries_seen = (unsigned long)idx->count;
  g_search.dirs_scanned = idx->dirs_scanned;
  g_search.files_indexed = idx->files_indexed;
  g_search.dirs_indexed = idx->dirs_indexed;
  g_search.skipped = idx->skipped;
  g_search.errors = idx->errors;
  g_search.memory_estimate = idx->memory_estimate;
  g_search.truncated = idx->truncated;
  g_search.last_errno = last_errno;
  if(current) snprintf(g_search.current, sizeof(g_search.current), "%s", current);
  if(error && error[0]) snprintf(g_search.error, sizeof(g_search.error), "%s", error);
  pthread_mutex_unlock(&g_search.lock);
}


static void
finish_build(search_index_t *idx, int rc, const char *error, int saved_errno,
             unsigned long start_generation) {
  search_index_t *old = NULL;
  long now_ms = search_monotonic_ms();
  unsigned long log_entries = 0;
  unsigned long log_dirs = 0;
  unsigned long log_files = 0;
  long log_elapsed_ms = 0;
  int log_stale = 0;

  pthread_mutex_lock(&g_search.lock);
  g_search.running = 0;
  g_search.cancel = 0;
  g_search.ended_at = time(NULL);
  g_search.elapsed_ms = now_ms > 0 && g_search.elapsed_ms > 0
                            ? now_ms - g_search.elapsed_ms
                            : 0;
  g_search.last_errno = saved_errno;
  g_search.entries_seen = idx ? (unsigned long)idx->count : g_search.entries_seen;
  g_search.dirs_scanned = idx ? idx->dirs_scanned : g_search.dirs_scanned;
  g_search.files_indexed = idx ? idx->files_indexed : g_search.files_indexed;
  g_search.dirs_indexed = idx ? idx->dirs_indexed : g_search.dirs_indexed;
  g_search.skipped = idx ? idx->skipped : g_search.skipped;
  g_search.errors = idx ? idx->errors : g_search.errors;
  g_search.memory_estimate = idx ? idx->memory_estimate : g_search.memory_estimate;
  g_search.truncated = idx ? idx->truncated : g_search.truncated;
  g_search.current[0] = 0;

  /* Publish any non-empty index even when some roots failed (rc==0 path). */
  if(rc == 0 && idx && idx->count > 0) {
    idx->built_at = g_search.ended_at;
    idx->build_ms = g_search.elapsed_ms;
    old = g_search.index;
    g_search.index = idx;
    g_search.stale = g_search.stale_generation != start_generation;
    if(!g_search.stale) g_search.stale_reason[0] = 0;
    if(error && error[0]) {
      snprintf(g_search.error, sizeof(g_search.error), "%s", error);
    } else {
      g_search.error[0] = 0;
    }
    search_save_index(idx);
  } else {
    if(error && error[0]) snprintf(g_search.error, sizeof(g_search.error), "%s", error);
    if(idx) {
      search_index_free(idx);
      idx = NULL;
    }
  }
  log_entries = g_search.entries_seen;
  log_dirs = g_search.dirs_scanned;
  log_files = g_search.files_indexed;
  log_elapsed_ms = g_search.elapsed_ms;
  log_stale = g_search.stale;
  pthread_mutex_unlock(&g_search.lock);

  search_index_free(old);
  bfpilot_log("search rebuild end rc=%d errno=%d entries=%lu dirs=%lu "
              "files=%lu elapsed_ms=%ld stale=%d error=%s",
              rc, saved_errno, log_entries, log_dirs, log_files,
              log_elapsed_ms, log_stale, error ? error : "");
  /* Agent-reachable summary line for post-session diagnosis. */
  {
    double eps = log_elapsed_ms > 0
                     ? ((double)log_entries * 1000.0) / (double)log_elapsed_ms
                     : 0.0;
    bfpilot_log("search rebuild stats unique_entries=%lu dirs_scanned=%lu "
                "files=%lu rate_entries_per_s=%.1f build_ms=%ld",
                log_entries, log_dirs, log_files, eps, log_elapsed_ms);
  }
}


typedef struct {
  dev_t dev;
  ino_t ino;
  unsigned char used; /* required: PS5 can report st_dev==0 (e.g. /user views) */
} visited_dir_t;

typedef struct {
  visited_dir_t *entries;
  size_t count;
  size_t cap;
} visited_set_t;

static size_t
visited_hash(dev_t dev, ino_t ino, size_t cap) {
  uint64_t mixed = ((uint64_t)dev * 0x9E3779B97F4A7C15ULL) ^
                   ((uint64_t)ino * 0xBF58476D1CE4E5B9ULL);
  return (size_t)(mixed & (cap - 1));
}

static int
visited_set_add(visited_set_t *set, dev_t dev, ino_t ino) {
  if(set->cap == 0 || set->count * 2 >= set->cap) {
    size_t new_cap = set->cap ? set->cap * 2 : 16384;
    visited_dir_t *new_entries = calloc(new_cap, sizeof(visited_dir_t));
    if(!new_entries) return -1;
    for(size_t i = 0; i < set->cap; i++) {
      if(!set->entries[i].used) continue;
      size_t idx = visited_hash(set->entries[i].dev, set->entries[i].ino, new_cap);
      while(new_entries[idx].used) {
        idx = (idx + 1) & (new_cap - 1);
      }
      new_entries[idx] = set->entries[i];
    }
    free(set->entries);
    set->entries = new_entries;
    set->cap = new_cap;
  }
  size_t idx = visited_hash(dev, ino, set->cap);
  while(set->entries[idx].used) {
    if(set->entries[idx].dev == dev && set->entries[idx].ino == ino) return 0;
    idx = (idx + 1) & (set->cap - 1);
  }
  set->entries[idx].dev = dev;
  set->entries[idx].ino = ino;
  set->entries[idx].used = 1;
  set->count++;
  return 1;
}

static void
visited_set_free(visited_set_t *set) {
  free(set->entries);
  set->entries = NULL;
  set->count = 0;
  set->cap = 0;
}


static int
crawl_queue_push(char ***dirs, size_t *count, size_t *cap, const char *path) {
  if(*count >= *cap) {
    size_t next = *cap ? (*cap * 2) : 256;
    char **items = (char **)realloc(*dirs, next * sizeof(*items));
    if(!items) return -1;
    *dirs = items;
    *cap = next;
  }
  char *copy = search_strdup(path);
  if(!copy) return -1;
  (*dirs)[(*count)++] = copy;
  return 0;
}

static int
crawl_one_root(search_index_t *idx, const char *root, char *error,
               size_t error_sz, int *saved_errno, int system_mode,
               visited_set_t *visited, int same_device) {
  (void)system_mode;
  struct stat root_st;
  if(lstat(root, &root_st) != 0) {
    *saved_errno = errno;
    snprintf(error, error_sz, "%s", strerror(*saved_errno));
    return -1;
  }
  if(!S_ISDIR(root_st.st_mode)) {
    *saved_errno = ENOTDIR;
    snprintf(error, error_sz, "%s", "search root is not a directory");
    return -1;
  }

  /* Already covered (same mount identity) — skip re-walk. */
  int vis = visited_set_add(visited, root_st.st_dev, root_st.st_ino);
  if(vis == 0) {
    search_diag_log("ROOT SKIP (already covered): %s", root);
    return 0;
  }
  if(vis < 0) {
    *saved_errno = ENOMEM;
    snprintf(error, error_sz, "%s", "visited-set out of memory");
    return -1;
  }

  if(search_index_add(idx, root, &root_st) < 0) {
    *saved_errno = ENOMEM;
    snprintf(error, error_sz, "%s", "out of memory");
    return -1;
  }
  if(idx->truncated) return 0;

  const dev_t root_dev = root_st.st_dev;
  char **dirs = NULL;
  size_t count = 0, cap = 0, head = 0;
  unsigned long root_errors = 0;

  if(crawl_queue_push(&dirs, &count, &cap, root) != 0) {
    *saved_errno = ENOMEM;
    snprintf(error, error_sz, "%s", "out of memory");
    return -1;
  }

  while(head < count && !search_cancelled() && !idx->truncated) {
    char *dir_path = dirs[head++];
    DIR *dir = opendir(dir_path);
    if(!dir) {
      idx->errors++;
      free(dir_path);
      continue;
    }

    idx->dirs_scanned++;
    if((idx->dirs_scanned % BFPILOT_SEARCH_PROGRESS_INTERVAL) == 0) {
      publish_progress(idx, dir_path, NULL, 0);
    }

    struct dirent *ent;
    for(;;) {
      errno = 0;
      ent = readdir(dir);
      if(!ent) {
        if(errno != 0) idx->errors++;
        break;
      }
      if(!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
      if(search_cancelled()) break;

      char child[1024];
      if(strlen(dir_path) + strlen(ent->d_name) + 2 >= sizeof(child)) {
        idx->skipped++;
        continue;
      }
      join_path(child, sizeof(child), dir_path, ent->d_name);

      if(search_skip_system_path(child)) {
        idx->skipped++;
        continue;
      }

      struct stat st = {0};
      if(lstat(child, &st) != 0) {
        int e = errno;
        idx->errors++;
        if(e == EIO || e == ESTALE || e == EFAULT || e == EBADF) {
          root_errors++;
          if(root_errors >= BFPILOT_SEARCH_ROOT_ERR_ABORT) {
            *saved_errno = e;
            snprintf(error, error_sz, "too many fatal FS errors under root");
            closedir(dir);
            free(dir_path);
            for(size_t i = head; i < count; i++) free(dirs[i]);
            free(dirs);
            return -1;
          }
        }
        continue;
      }

      /* XDEV fence: stay on this root's mount. Completeness for Index All
       * comes from multi-root collection (each mount is its own root) + a
       * shared visited set — not from blind cross-mount walks that re-enter
       * nullfs aliases and risk volatile mounts. */
      if(same_device && S_ISDIR(st.st_mode) && st.st_dev != root_dev) {
        idx->skipped++;
        (void)search_index_add(idx, child, &st);
        continue;
      }

      if(search_index_add(idx, child, &st) < 0) {
        *saved_errno = ENOMEM;
        snprintf(error, error_sz, "%s", "out of memory");
        closedir(dir);
        free(dir_path);
        for(size_t i = head; i < count; i++) free(dirs[i]);
        free(dirs);
        return -1;
      }

      if(S_ISDIR(st.st_mode) && !idx->truncated) {
        int v = visited_set_add(visited, st.st_dev, st.st_ino);
        if(v > 0) {
          if(crawl_queue_push(&dirs, &count, &cap, child) != 0) {
            *saved_errno = ENOMEM;
            snprintf(error, error_sz, "%s", "out of memory");
            closedir(dir);
            free(dir_path);
            for(size_t i = head; i < count; i++) free(dirs[i]);
            free(dirs);
            return -1;
          }
        } else if(v < 0) {
          *saved_errno = ENOMEM;
          snprintf(error, error_sz, "%s", "visited-set out of memory");
          closedir(dir);
          free(dir_path);
          for(size_t i = head; i < count; i++) free(dirs[i]);
          free(dirs);
          return -1;
        }
      }

      if((idx->count % BFPILOT_SEARCH_PROGRESS_INTERVAL) == 0) {
        publish_progress(idx, child, NULL, 0);
      }
    }

    closedir(dir);
    free(dir_path);
  }

  for(size_t i = head; i < count; i++) free(dirs[i]);
  free(dirs);

  if(search_cancelled()) {
    *saved_errno = ECANCELED;
    snprintf(error, error_sz, "%s", "cancelled");
    return -1;
  }

  publish_progress(idx, root, NULL, 0);
  return 0;
}


static void *
search_worker(void *arg) {
  crawler_arg_t *a = arg;
  char requested_root[1024];
  int global = a->global;
  int system = a->system;
  unsigned long start_generation = a->start_stale_generation;
  snprintf(requested_root, sizeof(requested_root), "%s", a->root);
  free(a);

  /* Let the accept/rebuild HTTP response finish before heavy FS work. */
  usleep(100000);

  (void)setpriority(PRIO_PROCESS, 0, BFPILOT_SEARCH_CRAWL_NICE);

  search_index_t *idx = calloc(1, sizeof(*idx));
  if(!idx) {
    finish_build(NULL, -1, "out of memory", ENOMEM, start_generation);
    return NULL;
  }

  /* Heap, not stack: BFPILOT_SEARCH_MAX_ROOTS * 1024 ≈ 64KiB overflows typical
   * PS5 pthread stacks and was killing the process right after rebuild accept. */
  char (*roots)[1024] = calloc((size_t)BFPILOT_SEARCH_MAX_ROOTS, 1024);
  if(!roots) {
    finish_build(idx, -1, "out of memory", ENOMEM, start_generation);
    return NULL;
  }

  int root_count = 0;
  /* Index All: multi-root discovery + ONE shared visited set (no double-count).
   * Cross-mount walk from each root (same_device=0 for global) recovers full
   * coverage; visited(dev,ino) stops re-walking aliases. */
  if(system || global) {
    root_count = collect_global_roots(roots);
    snprintf(idx->root, sizeof(idx->root), "%s",
             system ? BFPILOT_SEARCH_SYSTEM_ROOT_LABEL
                    : BFPILOT_SEARCH_ALL_ROOTS_LABEL);
  } else {
    snprintf(roots[root_count++], sizeof(roots[0]), "%s", requested_root);
    snprintf(idx->root, sizeof(idx->root), "%s", requested_root);
  }

  if(root_count <= 0) {
    free(roots);
    finish_build(idx, -1, "no searchable roots found", ENOENT,
                 start_generation);
    return NULL;
  }

  int rc = 0;
  int saved_errno = 0;
  char error[256] = {0};
  char roots_ok[768] = {0};
  char roots_fail[512] = {0};
  int roots_succeeded = 0;
  int roots_failed = 0;
  int fatal = 0;

  /* One global visited set across roots so nullfs aliases do not double-count. */
  visited_set_t visited = {0};

  for(int i = 0; i < root_count && !search_cancelled() && !idx->truncated && !fatal; i++) {
    publish_progress(idx, roots[i], NULL, 0);
    search_diag_log("ROOT CRAWL start: %s", roots[i]);
    /* Always same-device per root. Index All completeness = multi-root list
     * (/, /data, /user, usb/ext, system_*, …) + shared visited(dev,ino).
     * Cross-mount walks re-hit aliases and caused thrash/hang risk under P2JB. */
    int same_device = 1;
    int root_rc = crawl_one_root(idx, roots[i], error, sizeof(error), &saved_errno,
                                 system || global, &visited, same_device);
    if(root_rc == 0) {
      roots_succeeded++;
      append_root_csv(roots_ok, sizeof(roots_ok), roots[i]);
      search_diag_log("ROOT CRAWL ok: %s entries=%zu", roots[i], idx->count);
    } else if(saved_errno == ENOMEM) {
      /* Hard stop: do not thrash after OOM. */
      fatal = 1;
      rc = -1;
      roots_failed++;
      append_root_csv(roots_fail, sizeof(roots_fail), roots[i]);
      search_diag_log("ROOT CRAWL oom: %s", roots[i]);
    } else {
      /* Soft-fail: keep indexing remaining roots. */
      roots_failed++;
      append_root_csv(roots_fail, sizeof(roots_fail), roots[i]);
      search_diag_log("ROOT CRAWL fail: %s errno=%d err=%s", roots[i],
                      saved_errno, error);
      error[0] = 0;
      saved_errno = 0;
    }

    pthread_mutex_lock(&g_search.lock);
    g_search.roots_tried = i + 1;
    g_search.roots_succeeded = roots_succeeded;
    g_search.roots_failed = roots_failed;
    snprintf(g_search.roots_ok, sizeof(g_search.roots_ok), "%s", roots_ok);
    snprintf(g_search.roots_fail, sizeof(g_search.roots_fail), "%s", roots_fail);
    pthread_mutex_unlock(&g_search.lock);
  }

  visited_set_free(&visited);

  if(search_cancelled()) {
    if(idx->count > 0) {
      /* Keep partial index on cancel — still useful. */
      rc = 0;
      snprintf(error, sizeof(error), "%s", "cancelled (partial index kept)");
    } else {
      rc = -1;
      saved_errno = ECANCELED;
      snprintf(error, sizeof(error), "%s", "cancelled");
    }
  } else if(!fatal && idx->count > 0) {
    /* Partial success is success for Index All. */
    rc = 0;
    if(roots_failed > 0 && !error[0]) {
      snprintf(error, sizeof(error),
               "indexed with %d root failure(s)", roots_failed);
    }
  } else if(!fatal && idx->count == 0) {
    rc = -1;
    if(!error[0]) snprintf(error, sizeof(error), "%s", "no entries indexed");
    if(!saved_errno) saved_errno = ENOENT;
  }

  pthread_mutex_lock(&g_search.lock);
  g_search.roots_tried = root_count;
  g_search.roots_succeeded = roots_succeeded;
  g_search.roots_failed = roots_failed;
  snprintf(g_search.roots_ok, sizeof(g_search.roots_ok), "%s", roots_ok);
  snprintf(g_search.roots_fail, sizeof(g_search.roots_fail), "%s", roots_fail);
  pthread_mutex_unlock(&g_search.lock);

  search_diag_log("ROOT CRAWL summary ok=%d fail=%d entries=%zu",
                  roots_succeeded, roots_failed, idx->count);

  free(roots);

  publish_progress(idx, NULL, error[0] ? error : NULL, saved_errno);
  finish_build(idx, rc, error, saved_errno, start_generation);
  return NULL;
}


static int
append_status_json(json_buf_t *b) {
  search_index_t *idx;
  int indexed;
  int running, stale, cancel, truncated, last_errno;
  char root[1024], current[1024], error[256], stale_reason[96], last_query[128];
  time_t started_at, ended_at, built_at;
  long elapsed_ms, last_query_ms;
  unsigned long entries_seen, dirs_scanned, files_indexed, dirs_indexed;
  unsigned long skipped, errors, last_query_scanned, last_query_matched;
  unsigned long last_query_returned;
  unsigned long long memory_estimate;
  char roots_ok[768], roots_fail[512];
  int roots_tried, roots_succeeded, roots_failed;

  pthread_mutex_lock(&g_search.lock);
  idx = g_search.index;
  indexed = idx != NULL;
  running = g_search.running;
  stale = g_search.stale;
  cancel = g_search.cancel;
  truncated = g_search.truncated;
  last_errno = g_search.last_errno;
  snprintf(root, sizeof(root), "%s", g_search.root);
  snprintf(current, sizeof(current), "%s", g_search.current);
  snprintf(error, sizeof(error), "%s", g_search.error);
  snprintf(stale_reason, sizeof(stale_reason), "%s", g_search.stale_reason);
  snprintf(last_query, sizeof(last_query), "%s", g_search.last_query);
  snprintf(roots_ok, sizeof(roots_ok), "%s", g_search.roots_ok);
  snprintf(roots_fail, sizeof(roots_fail), "%s", g_search.roots_fail);
  roots_tried = g_search.roots_tried;
  roots_succeeded = g_search.roots_succeeded;
  roots_failed = g_search.roots_failed;
  started_at = g_search.started_at;
  ended_at = g_search.ended_at;
  elapsed_ms = g_search.elapsed_ms;
  entries_seen = g_search.entries_seen;
  dirs_scanned = g_search.dirs_scanned;
  files_indexed = g_search.files_indexed;
  dirs_indexed = g_search.dirs_indexed;
  skipped = g_search.skipped;
  errors = g_search.errors;
  memory_estimate = g_search.memory_estimate;
  last_query_scanned = g_search.last_query_scanned;
  last_query_matched = g_search.last_query_matched;
  last_query_returned = g_search.last_query_returned;
  last_query_ms = g_search.last_query_ms;
  built_at = idx ? idx->built_at : 0;
  if(idx && !running) {
    entries_seen = (unsigned long)idx->count;
    dirs_scanned = idx->dirs_scanned;
    files_indexed = idx->files_indexed;
    dirs_indexed = idx->dirs_indexed;
    skipped = idx->skipped;
    errors = idx->errors;
    memory_estimate = idx->memory_estimate;
    truncated = idx->truncated;
    snprintf(root, sizeof(root), "%s", idx->root);
    elapsed_ms = idx->build_ms;
  }
  if(running && started_at > 0) {
    long now = search_monotonic_ms();
    if(now > elapsed_ms) elapsed_ms = now - elapsed_ms;
  }
  pthread_mutex_unlock(&g_search.lock);

  if(json_append(b, "{\"ok\":true,\"running\":") != 0 ||
     json_append(b, running ? "true" : "false") != 0 ||
     json_append(b, ",\"cancelling\":") != 0 ||
     json_append(b, cancel ? "true" : "false") != 0 ||
     json_append(b, ",\"indexed\":") != 0 ||
     json_append(b, indexed ? "true" : "false") != 0 ||
     json_append(b, ",\"stale\":") != 0 ||
     json_append(b, stale ? "true" : "false") != 0 ||
     json_append(b, ",\"truncated\":") != 0 ||
     json_append(b, truncated ? "true" : "false") != 0 ||
     json_append(b, ",\"root\":") != 0 ||
     json_string(b, root) != 0 ||
     json_append(b, ",\"current\":") != 0 ||
     json_string(b, current) != 0 ||
     json_append(b, ",\"error\":") != 0 ||
     json_string(b, error) != 0 ||
     json_append(b, ",\"staleReason\":") != 0 ||
     json_string(b, stale_reason) != 0 ||
     json_appendf(b,
                  ",\"entries\":%lu,\"files\":%lu,\"dirs\":%lu,"
                  "\"dirsScanned\":%lu,\"skipped\":%lu,\"errors\":%lu,"
                  "\"memoryBytes\":%llu,\"startedAt\":%ld,\"endedAt\":%ld,"
                  "\"builtAt\":%ld,\"elapsedMs\":%ld,\"errno\":%d,"
                  "\"maxEntries\":%lu,\"lastQuery\":",
                  entries_seen, files_indexed, dirs_indexed, dirs_scanned,
                  skipped, errors, memory_estimate, (long)started_at,
                  (long)ended_at, (long)built_at, elapsed_ms, last_errno,
                  (unsigned long)BFPILOT_SEARCH_MAX_ENTRIES) != 0 ||
     json_string(b, last_query) != 0 ||
     json_appendf(b,
                  ",\"lastQueryScanned\":%lu,\"lastQueryMatched\":%lu,"
                  "\"lastQueryReturned\":%lu,\"lastQueryMs\":%ld,"
                  "\"rootsTried\":%d,\"rootsSucceeded\":%d,\"rootsFailed\":%d,"
                  "\"rootsOk\":",
                  last_query_scanned, last_query_matched,
                  last_query_returned, last_query_ms,
                  roots_tried, roots_succeeded, roots_failed) != 0 ||
     json_string(b, roots_ok) != 0 ||
     json_append(b, ",\"rootsFail\":") != 0 ||
     json_string(b, roots_fail) != 0 ||
     json_append(b, "}") != 0) {
    return -1;
  }
  return 0;
}


static int
status_request(const http_request_t *req) {
  json_buf_t b = {0};
  if(append_status_json(&b) != 0) {
    free(b.data);
    return -1;
  }
  return serve_owned(req, 200, b.data, b.len);
}


static int
rebuild_request(const http_request_t *req) {
  char root[1024];
  char mode[32] = {0};
  if(!websrv_get_query_arg(req, "root", root, sizeof(root))) {
    snprintf(root, sizeof(root), "%s", BFPILOT_SEARCH_DEFAULT_ROOT);
  }
  (void)websrv_get_query_arg(req, "mode", mode, sizeof(mode));
  int system = search_system_root_label(root) ||
               !strcmp(mode, BFPILOT_SEARCH_SYSTEM_ROOT_LABEL) ||
               !strcmp(mode, "root") ||
               !strcmp(mode, "everything");
  int global = !system && search_global_root_label(root);

  if(system) {
    snprintf(root, sizeof(root), "%s", BFPILOT_SEARCH_SYSTEM_ROOT_LABEL);
  } else if(global) {
    snprintf(root, sizeof(root), "%s", BFPILOT_SEARCH_ALL_ROOTS_LABEL);
  } else {
    if(!search_wide_path_allowed(root) || search_skip_system_path(root)) {
      return serve_error(req, 400, "search root is not allowed");
    }

    struct stat st;
    if(lstat(root, &st) != 0) return serve_error(req, 404, strerror(errno));
    if(!S_ISDIR(st.st_mode)) return serve_error(req, 400, "search root is not a directory");
  }

  crawler_arg_t *arg = calloc(1, sizeof(*arg));
  if(!arg) return serve_error(req, 500, "out of memory");
  snprintf(arg->root, sizeof(arg->root), "%s", root);
  arg->global = global;
  arg->system = system;

  pthread_mutex_lock(&g_search.lock);
  if(g_search.running) {
    pthread_mutex_unlock(&g_search.lock);
    free(arg);
    return serve_error(req, 409, "search rebuild is already running");
  }
  g_search.running = 1;
  g_search.cancel = 0;
  g_search.truncated = 0;
  g_search.last_errno = 0;
  g_search.started_at = time(NULL);
  g_search.ended_at = 0;
  g_search.elapsed_ms = search_monotonic_ms();
  g_search.entries_seen = 0;
  g_search.dirs_scanned = 0;
  g_search.files_indexed = 0;
  g_search.dirs_indexed = 0;
  g_search.skipped = 0;
  g_search.errors = 0;
  g_search.mount_skips = 0;
  g_search.memory_estimate = 0;
  g_search.roots_tried = 0;
  g_search.roots_succeeded = 0;
  g_search.roots_failed = 0;
  g_search.roots_ok[0] = 0;
  g_search.roots_fail[0] = 0;
  g_search.roots_skip[0] = 0;
  g_search.current[0] = 0;
  g_search.error[0] = 0;
  snprintf(g_search.root, sizeof(g_search.root), "%s", root);
  g_search.build_generation++;
  arg->start_stale_generation = g_search.stale_generation;
  pthread_mutex_unlock(&g_search.lock);

  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  /* Generous stack: PS5 defaults are tight; crawl locals + libc need room. */
  (void)pthread_attr_setstacksize(&at, 512 * 1024);
  int trc = pthread_create(&t, &at, search_worker, arg);
  pthread_attr_destroy(&at);
  if(trc != 0) {
    pthread_mutex_lock(&g_search.lock);
    g_search.running = 0;
    g_search.last_errno = trc;
    snprintf(g_search.error, sizeof(g_search.error), "%s", "pthread_create");
    pthread_mutex_unlock(&g_search.lock);
    free(arg);
    return serve_error(req, 500, "could not start search rebuild");
  }

  bfpilot_log("search rebuild start root=%s global=%d system=%d",
              root, global, system);
  return status_request(req);
}


static int
cancel_request(const http_request_t *req) {
  pthread_mutex_lock(&g_search.lock);
  if(g_search.running) g_search.cancel = 1;
  pthread_mutex_unlock(&g_search.lock);
  return status_request(req);
}


static unsigned long
parse_ulong_arg(const http_request_t *req, const char *name,
                unsigned long fallback, unsigned long max_value) {
  char value[32];
  if(!websrv_get_query_arg(req, name, value, sizeof(value))) return fallback;
  char *end = NULL;
  errno = 0;
  unsigned long out = strtoul(value, &end, 10);
  if(errno != 0 || end == value || (end && *end)) return fallback;
  if(out > max_value) out = max_value;
  return out;
}


static int
split_query(char *query, char **terms, int max_terms) {
  int count = 0;
  char *p = query;
  while(*p && count < max_terms) {
    while(*p && isspace((unsigned char)*p)) p++;
    if(!*p) break;
    terms[count++] = p;
    while(*p && !isspace((unsigned char)*p)) p++;
    if(*p) *p++ = 0;
  }
  return count;
}


static int
entry_matches(const search_entry_t *entry, char **terms, int term_count,
              const char *root, int type_filter, int scope_name) {
  if(root && root[0] && !path_starts_with_root(entry->path, root)) return 0;
  if(type_filter == 1 && entry->dir) return 0;
  if(type_filter == 2 && !entry->dir) return 0;

  const char *haystack = scope_name ? entry->path + entry->name_offset
                                    : entry->path;
  for(int i = 0; i < term_count; i++) {
    if(!strcasestr(haystack, terms[i])) return 0;
  }
  return 1;
}


static int
query_request(const http_request_t *req) {
  char query[BFPILOT_SEARCH_MAX_QUERY];
  char root[1024] = {0};
  char type[32];
  char scope[32];

  if(!websrv_get_query_arg(req, "q", query, sizeof(query)) || !query[0]) {
    return serve_error(req, 400, "missing search query");
  }
  (void)websrv_get_query_arg(req, "root", root, sizeof(root));
  if(search_global_root_label(root) || search_system_root_label(root)) {
    root[0] = 0;
  } else if(root[0] && !search_wide_path_allowed(root)) {
    return serve_error(req, 400, "search root is not allowed");
  }
  if(!websrv_get_query_arg(req, "type", type, sizeof(type))) {
    snprintf(type, sizeof(type), "%s", "all");
  }
  if(!websrv_get_query_arg(req, "scope", scope, sizeof(scope))) {
    snprintf(scope, sizeof(scope), "%s", "path");
  }
  int type_filter = 0;
  if(!strcmp(type, "file") || !strcmp(type, "files")) type_filter = 1;
  else if(!strcmp(type, "dir") || !strcmp(type, "dirs") ||
          !strcmp(type, "folder") || !strcmp(type, "folders")) type_filter = 2;
  int scope_name = !strcmp(scope, "name") || !strcmp(scope, "filename");

  unsigned long limit = parse_ulong_arg(req, "limit", BFPILOT_SEARCH_DEFAULT_LIMIT,
                                        BFPILOT_SEARCH_MAX_LIMIT);
  unsigned long offset = parse_ulong_arg(req, "offset", 0, 10000000UL);
  if(limit == 0) limit = BFPILOT_SEARCH_DEFAULT_LIMIT;

  char query_buffer[BFPILOT_SEARCH_MAX_QUERY];
  snprintf(query_buffer, sizeof(query_buffer), "%s", query);
  char *terms[BFPILOT_SEARCH_MAX_TERMS];
  int term_count = split_query(query_buffer, terms, BFPILOT_SEARCH_MAX_TERMS);
  if(term_count == 0) return serve_error(req, 400, "missing search query");

  json_buf_t b = {0};
  long started_ms = search_monotonic_ms();
  unsigned long scanned = 0;
  unsigned long matched = 0;
  unsigned long returned = 0;
  int first = 1;
  int stale = 0;
  int truncated = 0;
  char indexed_root[1024] = {0};

  pthread_mutex_lock(&g_search.lock);
  search_index_t *idx = g_search.index;
  if(!idx) {
    pthread_mutex_unlock(&g_search.lock);
    return serve_error(req, 409, "search index is empty; rebuild first");
  }
  stale = g_search.stale;
  truncated = idx->truncated;
  snprintf(indexed_root, sizeof(indexed_root), "%s", idx->root);

  if(json_append(&b, "{\"ok\":true,\"query\":") != 0 ||
     json_string(&b, query) != 0 ||
     json_append(&b, ",\"root\":") != 0 ||
     json_string(&b, root[0] ? root : indexed_root) != 0 ||
     json_append(&b, ",\"indexedRoot\":") != 0 ||
     json_string(&b, indexed_root) != 0 ||
     json_append(&b, ",\"scope\":") != 0 ||
     json_string(&b, scope_name ? "name" : "path") != 0 ||
     json_appendf(&b, ",\"stale\":%s,\"indexTruncated\":%s,\"offset\":%lu,"
                  "\"limit\":%lu,\"results\":[",
                  stale ? "true" : "false",
                  truncated ? "true" : "false", offset, limit) != 0) {
    pthread_mutex_unlock(&g_search.lock);
    free(b.data);
    return -1;
  }

  for(size_t i = 0; i < idx->count; i++) {
    const search_entry_t *entry = &idx->entries[i];
    scanned++;
    if(!entry_matches(entry, terms, term_count, root, type_filter, scope_name)) {
      continue;
    }
    matched++;
    if(matched <= offset) continue;
    if(returned >= limit) continue;

    int json_ok = 1;
    if(!first && json_append(&b, ",") != 0) json_ok = 0;
    first = 0;
    const char *name = entry->path + entry->name_offset;
    if(!json_ok ||
       json_append(&b, "{\"path\":") != 0 ||
       json_string(&b, entry->path) != 0 ||
       json_append(&b, ",\"name\":") != 0 ||
       json_string(&b, name) != 0 ||
       json_appendf(&b, ",\"dir\":%s,\"size\":%llu,\"mtime\":%ld}",
                    entry->dir ? "true" : "false", entry->size,
                    entry->mtime) != 0) {
      pthread_mutex_unlock(&g_search.lock);
      free(b.data);
      return -1;
    }
    returned++;
  }
  long elapsed_ms = search_monotonic_ms() - started_ms;

  if(json_appendf(&b,
                  "],\"scanned\":%lu,\"matched\":%lu,\"returned\":%lu,"
                  "\"hasMore\":%s,\"elapsedMs\":%ld}",
                  scanned, matched, returned,
                  matched > offset + returned ? "true" : "false",
                  elapsed_ms > 0 ? elapsed_ms : 0) != 0) {
    pthread_mutex_unlock(&g_search.lock);
    free(b.data);
    return -1;
  }

  snprintf(g_search.last_query, sizeof(g_search.last_query), "%s", query);
  g_search.last_query_scanned = scanned;
  g_search.last_query_matched = matched;
  g_search.last_query_returned = returned;
  g_search.last_query_ms = elapsed_ms > 0 ? elapsed_ms : 0;
  pthread_mutex_unlock(&g_search.lock);

  return serve_owned(req, 200, b.data, b.len);
}


void
bfpilot_search_mark_stale(const char *reason) {
  pthread_mutex_lock(&g_search.lock);
  g_search.stale = 1;
  g_search.stale_generation++;
  snprintf(g_search.stale_reason, sizeof(g_search.stale_reason), "%s",
           reason && reason[0] ? reason : "filesystem changed");
  pthread_mutex_unlock(&g_search.lock);
}


void
bfpilot_search_shutdown(void) {
  pthread_mutex_lock(&g_search.lock);
  g_search.cancel = 1;
  pthread_mutex_unlock(&g_search.lock);
  
  for(int i = 0; i < 50; i++) {
    pthread_mutex_lock(&g_search.lock);
    int running = g_search.running;
    pthread_mutex_unlock(&g_search.lock);
    if(!running) break;
    usleep(20000); // 20ms
  }
}

int
bfpilot_search_request(const http_request_t *req, const char *url) {
  int is_rebuild = url && !strcmp(url, "/api/fs/search/rebuild");
  /* Load disk cache without holding the search lock (avoid blocking HTTP).
   * Skip on rebuild — we are about to replace the index anyway. */
  int need_load = 0;
  pthread_mutex_lock(&g_search.lock);
  if(!is_rebuild && !g_search.index && !g_search.running) need_load = 1;
  pthread_mutex_unlock(&g_search.lock);
  if(need_load) {
    search_index_t *idx = search_load_index();
    if(idx) {
      pthread_mutex_lock(&g_search.lock);
      if(!g_search.index) {
        g_search.index = idx;
        g_search.stale = 0;
        g_search.stale_reason[0] = 0;
        g_search.stale_generation++;
        idx = NULL;
      }
      pthread_mutex_unlock(&g_search.lock);
      if(idx) search_index_free(idx);
    }
  }

  if(!strcmp(url, "/api/fs/search/status")) return status_request(req);
  if(is_rebuild) return rebuild_request(req);
  if(!strcmp(url, "/api/fs/search/cancel")) return cancel_request(req);
  if(!strcmp(url, "/api/fs/search")) return query_request(req);
  return serve_error(req, 404, "no such search endpoint");
}
