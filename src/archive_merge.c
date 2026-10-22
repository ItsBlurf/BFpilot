/* Complete-file publication without a second copy. The immutable manifest
   survives partial moves; all paths resolve through anchored nofollow fds. */
#include "archive_merge.h"
#include "fs_at.h"
#include "tree_chmod.h"
#include "tree_walk.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MERGE_PATH 1024U
#define MERGE_ID 128U
#define HEADER_SIZE 2244U
#define ENTRY_PREFIX 64U
#define ENTRY_MAX (ENTRY_PREFIX + MERGE_PATH - 1U + 4U)
enum { MERGE_DIR = 1, MERGE_FILE = 2, MERGE_LINK = 3 };
typedef struct {
  char stage[MERGE_PATH], destination[MERGE_PATH], id[MERGE_ID];
  uint64_t source_dev, source_ino, dest_dev, dest_ino, files, bytes;
  uint32_t entries;
} merge_header_t;
typedef struct {
  char relative[MERGE_PATH];
  uint64_t dev, ino, size;
  int64_t mtime;
  uint32_t nsec, kind;
} merge_entry_t;
typedef struct {
  merge_header_t header;
  const bfpilot_merge_options_t *options;
  bfpilot_merge_result_t *result;
  int source, destination;
  FILE *manifest;
  uint32_t body_crc;
  char stage_parent[MERGE_PATH], control_parent[MERGE_PATH];
} merge_context_t;

static uint32_t crc_update(uint32_t crc, const void *data, size_t n) {
  const unsigned char *p = data;
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}
static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static uint64_t get64(const unsigned char *p) {
  return (uint64_t)get32(p) << 32 | get32(p + 4);
}
static void put32(unsigned char *p, uint32_t v) {
  for (int i = 3; i >= 0; i--, v >>= 8)
    p[i] = (unsigned char)v;
}
static void put64(unsigned char *p, uint64_t v) {
  put32(p, (uint32_t)(v >> 32));
  put32(p + 4, (uint32_t)v);
}
static int closed(int fd, int rc) {
  if (close(fd) && !rc)
    return -errno;
  return rc;
}
static int stream_closed(FILE *f, int rc) {
  if (fclose(f) && !rc)
    return -(errno ? errno : EIO);
  return rc;
}
static int cancelled(void *opaque) {
  merge_context_t *c = opaque;
  return c->options && c->options->cancelled &&
         c->options->cancelled(c->options->context);
}
static int copy_text(char *out, size_t capacity, const char *text) {
  if (!text)
    return -EINVAL;
  size_t n = 0;
  while (n < capacity && text[n])
    n++;
  if (!n || n == capacity)
    return -ENAMETOOLONG;
  memcpy(out, text, n + 1);
  return 0;
}
static int segments_safe(const char *path, int absolute) {
  if (!path || (*path == '/') != absolute)
    return 0;
  const char *part = path + absolute;
  if (!*part)
    return absolute;
  for (const char *p = part;; p++) {
    if (*p && *p != '/')
      continue;
    size_t n = (size_t)(p - part);
    if (!n || (n == 1 && part[0] == '.') ||
        (n == 2 && part[0] == '.' && part[1] == '.'))
      return 0;
    if (!*p)
      return 1;
    part = p + 1;
  }
}
static int contains(const char *parent, const char *path) {
  size_t n = strlen(parent);
  return !strcmp(parent, "/") ||
         (!strncmp(parent, path, n) && (!path[n] || path[n] == '/'));
}
static int identity(const struct stat *st, uint64_t dev, uint64_t ino) {
  return (uint64_t)st->st_dev == dev && (uint64_t)st->st_ino == ino;
}
static unsigned kind(const struct stat *st) {
  return S_ISDIR(st->st_mode)   ? MERGE_DIR
         : S_ISREG(st->st_mode) ? MERGE_FILE
         : S_ISLNK(st->st_mode) ? MERGE_LINK
                                : 0;
}
static int entry_matches(const struct stat *st, const merge_entry_t *e) {
  return identity(st, e->dev, e->ino) && kind(st) == e->kind &&
         (e->kind == MERGE_DIR ||
          (st->st_size >= 0 && (uint64_t)st->st_size == e->size &&
           (int64_t)st->st_mtim.tv_sec == e->mtime &&
           (uint32_t)st->st_mtim.tv_nsec == e->nsec));
}
/* Open absolute roots without following an ancestor symlink. Mounts along the
   explicit root path are allowed; child traversal verifies the final volume. */
static int open_absolute(const char *path) {
  if (!segments_safe(path, 1) || strlen(path) >= MERGE_PATH)
    return -EINVAL;
  int fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (fd < 0)
    return -errno;
  for (const char *p = path + 1; *p;) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    char name[MERGE_PATH];
    memcpy(name, p, n);
    name[n] = 0;
    int next =
        bfpilot_fs_openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (next < 0) {
      int rc = -errno;
      close(fd);
      return rc;
    }
    int rc = closed(fd, 0);
    if (rc) {
      close(next);
      return rc;
    }
    fd = next;
    p = end ? end + 1 : p + n;
  }
  return fd;
}
/* Return an owned parent fd, leaving the leaf name in the caller's record. */
static int parent_fd(int root, const char *relative, uint64_t dev, int create) {
  if (root < 0)
    return -ENOENT;
  int fd = dup(root);
  if (fd < 0)
    return -errno;
  const char *p = relative, *slash;
  while ((slash = strchr(p, '/'))) {
    size_t n = (size_t)(slash - p);
    char name[MERGE_PATH];
    if (!n || n >= sizeof(name)) {
      close(fd);
      return -EINVAL;
    }
    memcpy(name, p, n);
    name[n] = 0;
    int next =
        bfpilot_fs_openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (next < 0 && errno == ENOENT && create) {
      if (bfpilot_fs_mkdirat(fd, name, 0700) && errno != EEXIST) {
        int rc = -errno;
        close(fd);
        return rc;
      }
      next =
          bfpilot_fs_openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    }
    if (next < 0) {
      int rc = -errno;
      close(fd);
      return rc;
    }
    struct stat st;
    int rc = fstat(next, &st) ? -errno : 0;
    if (!rc && (uint64_t)st.st_dev != dev)
      rc = -EXDEV;
    rc = closed(fd, rc);
    if (rc) {
      close(next);
      return rc;
    }
    fd = next;
    p = slash + 1;
  }
  return fd;
}
static const char *leaf(const char *relative) {
  const char *p = strrchr(relative, '/');
  return p ? p + 1 : relative;
}
static int entry_stat(int root, const char *relative, uint64_t dev,
                      struct stat *st) {
  int fd = parent_fd(root, relative, dev, 0);
  if (fd < 0)
    return fd;
  int rc = bfpilot_fs_fstatat(fd, leaf(relative), st, AT_SYMLINK_NOFOLLOW)
               ? -errno
               : 0;
  return closed(fd, rc);
}
static int record_encode(const merge_entry_t *e, unsigned char *out,
                         size_t *size) {
  size_t n = strlen(e->relative);
  if (!n || n >= MERGE_PATH || !segments_safe(e->relative, 0) || !e->ino ||
      e->kind < MERGE_DIR || e->kind > MERGE_LINK || e->nsec >= 1000000000U ||
      e->size > INT64_MAX)
    return -EINVAL;
  *size = ENTRY_PREFIX + n + 4;
  memset(out, 0, ENTRY_PREFIX);
  memcpy(out, "BFME", 4);
  put32(out + 4, e->kind);
  put32(out + 8, (uint32_t)*size);
  put32(out + 12, (uint32_t)n);
  put64(out + 16, e->dev);
  put64(out + 24, e->ino);
  put64(out + 32, e->size);
  put64(out + 40, (uint64_t)e->mtime);
  put32(out + 48, e->nsec);
  memcpy(out + ENTRY_PREFIX, e->relative, n);
  put32(out + ENTRY_PREFIX + n, ~crc_update(UINT32_MAX, out, ENTRY_PREFIX + n));
  return 0;
}
static int record_read(FILE *f, merge_entry_t *e, uint32_t *body_crc) {
  unsigned char bytes[ENTRY_MAX];
  if (fread(bytes, 1, ENTRY_PREFIX, f) != ENTRY_PREFIX)
    return -(ferror(f) && errno ? errno : EPROTO);
  size_t n = get32(bytes + 12), total = get32(bytes + 8);
  if (memcmp(bytes, "BFME", 4) || !n || n >= MERGE_PATH ||
      total != ENTRY_PREFIX + n + 4)
    return -EPROTO;
  for (unsigned i = 52; i < ENTRY_PREFIX; i++)
    if (bytes[i])
      return -EPROTO;
  if (fread(bytes + ENTRY_PREFIX, 1, n + 4, f) != n + 4)
    return -(ferror(f) && errno ? errno : EPROTO);
  if (memchr(bytes + ENTRY_PREFIX, 0, n) ||
      get32(bytes + ENTRY_PREFIX + n) !=
          ~crc_update(UINT32_MAX, bytes, ENTRY_PREFIX + n))
    return -EPROTO;
  memset(e, 0, sizeof(*e));
  memcpy(e->relative, bytes + ENTRY_PREFIX, n);
  e->kind = get32(bytes + 4);
  e->dev = get64(bytes + 16);
  e->ino = get64(bytes + 24);
  e->size = get64(bytes + 32);
  e->mtime = (int64_t)get64(bytes + 40);
  e->nsec = get32(bytes + 48);
  if (!segments_safe(e->relative, 0) || !e->ino || e->kind < MERGE_DIR ||
      e->kind > MERGE_LINK || e->nsec >= 1000000000U || e->size > INT64_MAX)
    return -EPROTO;
  if (body_crc)
    *body_crc = crc_update(*body_crc, bytes, total);
  return 0;
}
static void header_encode(const merge_header_t *h,
                          unsigned char out[HEADER_SIZE]) {
  memset(out, 0, HEADER_SIZE);
  memcpy(out, "BFMERGE1", 8);
  put32(out + 8, 1);
  put32(out + 12, h->entries);
  put64(out + 16, h->source_dev);
  put64(out + 24, h->source_ino);
  put64(out + 32, h->dest_dev);
  put64(out + 40, h->dest_ino);
  put64(out + 48, h->files);
  put64(out + 56, h->bytes);
  memcpy(out + 64, h->stage, strlen(h->stage));
  memcpy(out + 1088, h->destination, strlen(h->destination));
  memcpy(out + 2112, h->id, strlen(h->id));
  put32(out + 2240, ~crc_update(UINT32_MAX, out, 2240));
}
static int field_decode(char *out, const unsigned char *in, size_t capacity) {
  const unsigned char *end = memchr(in, 0, capacity);
  if (!end || end == in)
    return -EPROTO;
  size_t n = (size_t)(end - in);
  for (size_t i = n; i < capacity; i++)
    if (in[i])
      return -EPROTO;
  memcpy(out, in, n);
  out[n] = 0;
  return 0;
}
static int header_read(FILE *f, merge_header_t *h) {
  unsigned char bytes[HEADER_SIZE];
  if (fread(bytes, 1, sizeof(bytes), f) != sizeof(bytes))
    return -(ferror(f) && errno ? errno : EPROTO);
  if (memcmp(bytes, "BFMERGE1", 8) || get32(bytes + 8) != 1 ||
      get32(bytes + 2240) != ~crc_update(UINT32_MAX, bytes, 2240))
    return -EPROTO;
  memset(h, 0, sizeof(*h));
  h->entries = get32(bytes + 12);
  h->source_dev = get64(bytes + 16);
  h->source_ino = get64(bytes + 24);
  h->dest_dev = get64(bytes + 32);
  h->dest_ino = get64(bytes + 40);
  h->files = get64(bytes + 48);
  h->bytes = get64(bytes + 56);
  if (h->entries > BFMERGE_MAX_ENTRIES || h->files > h->entries ||
      !h->source_ino || !h->dest_ino || h->source_dev != h->dest_dev ||
      field_decode(h->stage, bytes + 64, MERGE_PATH) ||
      field_decode(h->destination, bytes + 1088, MERGE_PATH) ||
      field_decode(h->id, bytes + 2112, MERGE_ID) ||
      !segments_safe(h->stage, 1) || !segments_safe(h->destination, 1))
    return -EPROTO;
  for (const char *p = h->id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return -EPROTO;
  return 0;
}
/* No mutation. Source absent is success only for an exactly identified output.
 */
static int validate_entry(merge_context_t *c, const merge_entry_t *e,
                          int *already) {
  *already = 0;
  char target[MERGE_PATH];
  int n = snprintf(target, sizeof(target), "%s/%s", c->header.destination,
                   e->relative);
  if (n < 0 || (size_t)n >= sizeof(target))
    return -ENAMETOOLONG;
  snprintf(c->result->error_path, sizeof(c->result->error_path), "%s", target);
  if (contains(c->stage_parent, target) || contains(c->control_parent, target))
    return -EINVAL;
  if (e->dev != c->header.source_dev)
    return -EXDEV;
  struct stat src, dst;
  int sr = entry_stat(c->source, e->relative, e->dev, &src);
  int dr = entry_stat(c->destination, e->relative, c->header.dest_dev, &dst);
  if (sr && sr != -ENOENT)
    return sr;
  if (dr && dr != -ENOENT)
    return dr;
  if (!dr && (uint64_t)dst.st_dev != c->header.dest_dev)
    return -EXDEV;
  if (c->options && c->options->protect) {
    int rc = c->options->protect(c->options->context, target, dr ? NULL : &dst);
    if (rc)
      return rc;
  }
  if (!sr && !entry_matches(&src, e))
    return -EAGAIN;
  if (e->kind == MERGE_DIR) {
    if (!dr && !S_ISDIR(dst.st_mode))
      return -EEXIST;
    if (sr && dr)
      return -ENOENT;
    *already = sr != 0;
    return 0;
  }
  if (!dr && S_ISDIR(dst.st_mode))
    return -EISDIR;
  if (sr) {
    if (dr || !entry_matches(&dst, e))
      return -ENOENT;
    *already = 1;
  }
  return 0;
}
static int manifest_entry(const char *path, const char *relative,
                          const struct stat *st, void *opaque) {
  (void)path;
  merge_context_t *c = opaque;
  if (!*relative)
    return 0;
  if (c->header.entries >= BFMERGE_MAX_ENTRIES)
    return -E2BIG;
  merge_entry_t e = {0};
  if (copy_text(e.relative, sizeof(e.relative), relative))
    return -ENAMETOOLONG;
  e.kind = kind(st);
  e.dev = (uint64_t)st->st_dev;
  e.ino = (uint64_t)st->st_ino;
  e.size = e.kind == MERGE_DIR ? 0
           : st->st_size < 0   ? UINT64_MAX
                               : (uint64_t)st->st_size;
  e.mtime = (int64_t)st->st_mtim.tv_sec;
  e.nsec = (uint32_t)st->st_mtim.tv_nsec;
  unsigned char bytes[ENTRY_MAX];
  size_t size = 0;
  int rc = record_encode(&e, bytes, &size);
  if (rc)
    return rc;
  int already = 0;
  rc = validate_entry(c, &e, &already);
  if (rc)
    return rc;
  if (fwrite(bytes, 1, size, c->manifest) != size)
    return -(errno ? errno : EIO);
  c->body_crc = crc_update(c->body_crc, bytes, size);
  c->header.entries++;
  if (e.kind == MERGE_FILE) {
    if (e.size > UINT64_MAX - c->header.bytes)
      return -EOVERFLOW;
    c->header.bytes += e.size;
    c->header.files++;
  }
  return 0;
}
static int prepare_manifest(merge_context_t *c, const char *name) {
  if (c->source < 0)
    return -ENOENT;
  bfpilot_tree_totals_t totals = {0};
  int rc =
      bfpilot_tree_finalize_stats(c->header.stage, 0777, cancelled, c, &totals);
  if (rc)
    return rc;
  char temporary[MERGE_PATH];
  int n = snprintf(temporary, sizeof(temporary), "%s.tmp", name);
  if (n < 0 || (size_t)n >= sizeof(temporary))
    return -ENAMETOOLONG;
  int fd = open(temporary, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -errno;
  FILE *f = fdopen(fd, "w+b");
  if (!f) {
    rc = -errno;
    close(fd);
    unlink(temporary);
    return rc;
  }
  c->manifest = f;
  c->body_crc = UINT32_MAX;
  unsigned char header[HEADER_SIZE];
  header_encode(&c->header, header);
  if (fwrite(header, 1, sizeof(header), f) != sizeof(header))
    rc = -(errno ? errno : EIO);
  if (!rc)
    rc = bfpilot_tree_walk(c->header.stage, (dev_t)c->header.source_dev,
                           manifest_entry, cancelled, c);
  if (!rc &&
      (totals.files != c->header.files || totals.bytes != c->header.bytes))
    rc = -EAGAIN;
  unsigned char footer[4];
  put32(footer, ~c->body_crc);
  if (!rc && fwrite(footer, 1, sizeof(footer), f) != sizeof(footer))
    rc = -(errno ? errno : EIO);
  if (!rc && fseek(f, 0, SEEK_SET))
    rc = -(errno ? errno : EIO);
  header_encode(&c->header, header);
  if (!rc && fwrite(header, 1, sizeof(header), f) != sizeof(header))
    rc = -(errno ? errno : EIO);
  if (!rc && fflush(f))
    rc = -(errno ? errno : EIO);
  if (!rc && fsync(fd))
    rc = -errno;
  rc = stream_closed(f, rc);
  c->manifest = NULL;
  if (!rc && rename(temporary, name))
    rc = -errno;
  if (rc)
    unlink(temporary);
  return rc;
}
static int read_manifest(FILE *f, merge_context_t *c, int validate_files) {
  merge_header_t h;
  int rc = header_read(f, &h);
  if (rc)
    return rc;
  if (strcmp(h.stage, c->header.stage) ||
      strcmp(h.destination, c->header.destination) ||
      strcmp(h.id, c->header.id) || h.dest_dev != c->header.dest_dev ||
      h.dest_ino != c->header.dest_ino ||
      (c->source >= 0 && (h.source_dev != c->header.source_dev ||
                          h.source_ino != c->header.source_ino)))
    return -EAGAIN;
  c->header = h;
  uint32_t crc = UINT32_MAX;
  uint64_t files = 0, bytes = 0;
  for (uint32_t i = 0; i < h.entries; i++) {
    if (cancelled(c))
      return -ECANCELED;
    merge_entry_t e;
    rc = record_read(f, &e, &crc);
    if (rc)
      return rc;
    if (e.dev != h.source_dev)
      return -EXDEV;
    if (e.kind == MERGE_FILE) {
      if (e.size > UINT64_MAX - bytes)
        return -EOVERFLOW;
      bytes += e.size;
      files++;
    }
    if (validate_files) {
      int already;
      rc = validate_entry(c, &e, &already);
      if (rc)
        return rc;
    }
  }
  unsigned char footer[4];
  if (fread(footer, 1, sizeof(footer), f) != sizeof(footer))
    return -(ferror(f) && errno ? errno : EPROTO);
  if (get32(footer) != ~crc || fgetc(f) != EOF || files != h.files ||
      bytes != h.bytes)
    return -EPROTO;
  if (ferror(f))
    return -(errno ? errno : EIO);
  return 0;
}
static int publish_entry(merge_context_t *c, const merge_entry_t *e) {
  int already = 0, rc = validate_entry(c, e, &already);
  if (rc || already)
    return rc;
  int dest = parent_fd(c->destination, e->relative, c->header.dest_dev, 1);
  if (dest < 0)
    return dest;
  if (e->kind == MERGE_DIR) {
    int created = bfpilot_fs_mkdirat(dest, leaf(e->relative), 0700) == 0;
    if (!created && errno != EEXIST)
      rc = -errno;
    int directory =
        rc ? -1
           : bfpilot_fs_openat(dest, leaf(e->relative),
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (!rc && directory < 0)
      rc = -errno;
    if (directory >= 0) {
      struct stat opened;
      if (fstat(directory, &opened))
        rc = -errno;
      else if ((uint64_t)opened.st_dev != c->header.dest_dev)
        rc = -EXDEV;
      else if (created && fchmod(directory, 0777))
        rc = -errno;
      rc = closed(directory, rc);
    }
  } else {
    int source = parent_fd(c->source, e->relative, e->dev, 0);
    if (source < 0)
      rc = source;
    else {
      if (bfpilot_fs_renameat(source, leaf(e->relative), dest,
                              leaf(e->relative)))
        rc = -errno;
      if (!rc) {
        struct stat moved;
        if (bfpilot_fs_fstatat(dest, leaf(e->relative), &moved,
                               AT_SYMLINK_NOFOLLOW))
          rc = -errno;
        else if (!entry_matches(&moved, e))
          rc = -EAGAIN;
        // POSIX hard-linked aliases can make rename a no-op. Remove only the
        // verified source alias, retaining the complete destination inode.
        if (!rc) {
          struct stat remaining;
          if (!bfpilot_fs_fstatat(source, leaf(e->relative), &remaining,
                                  AT_SYMLINK_NOFOLLOW)) {
            if (!entry_matches(&remaining, e))
              rc = -EAGAIN;
            else if (bfpilot_fs_unlinkat(source, leaf(e->relative), 0))
              rc = -errno;
          } else if (errno != ENOENT)
            rc = -errno;
        }
      }
      rc = closed(source, rc);
    }
  }
  return closed(dest, rc);
}
static int remove_empty_directory(const char *path, const char *relative,
                                  const struct stat *st, void *opaque) {
  (void)relative;
  (void)opaque;
  if (!S_ISDIR(st->st_mode))
    return -ENOTEMPTY;
  return bfpilot_fs_unlinkat(AT_FDCWD, path, AT_REMOVEDIR) ? -errno : 0;
}
static int context_open(merge_context_t *c, const char *stage,
                        const char *destination, const char *id,
                        const bfpilot_merge_options_t *options,
                        bfpilot_merge_result_t *result, const char *manifest) {
  memset(c, 0, sizeof(*c));
  c->source = c->destination = -1;
  c->result = result;
  c->options = options;
  if (copy_text(c->header.stage, sizeof(c->header.stage), stage) ||
      copy_text(c->header.destination, sizeof(c->header.destination),
                destination) ||
      copy_text(c->header.id, sizeof(c->header.id), id) ||
      !segments_safe(stage, 1) || !segments_safe(destination, 1))
    return -EINVAL;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
      return -EINVAL;
  snprintf(c->stage_parent, sizeof(c->stage_parent), "%s", stage);
  char *slash = strrchr(c->stage_parent, '/');
  if (!slash || slash == c->stage_parent)
    return -EINVAL;
  *slash = 0;
  if (contains(stage, destination) || contains(c->stage_parent, destination))
    return -EINVAL;
  if (copy_text(c->control_parent, sizeof(c->control_parent), manifest) ||
      !segments_safe(manifest, 1))
    return -EINVAL;
  char *control_slash = strrchr(c->control_parent, '/');
  if (!control_slash || control_slash == c->control_parent)
    return -EINVAL;
  *control_slash = 0;
  if (contains(stage, c->control_parent) ||
      contains(c->control_parent, destination))
    return -EINVAL;
  int control = open_absolute(c->control_parent);
  if (control < 0)
    return control;
  struct stat control_st;
  int control_rc = fstat(control, &control_st) ? -errno : 0;
  if (!control_rc &&
      ((control_st.st_mode & 077) ||
       (control_st.st_uid != geteuid() && control_st.st_uid != 0)))
    control_rc = -EACCES;
  control_rc = closed(control, control_rc);
  if (control_rc)
    return control_rc;
  c->destination = open_absolute(destination);
  if (c->destination < 0)
    return c->destination;
  struct stat dst, src;
  int rc = fstat(c->destination, &dst) ? -errno : 0;
  if (!rc) {
    c->header.dest_dev = (uint64_t)dst.st_dev;
    c->header.dest_ino = (uint64_t)dst.st_ino;
  }
  if (!rc) {
    c->source = open_absolute(stage);
    if (c->source < 0 && c->source != -ENOENT)
      rc = c->source;
    else if (c->source >= 0) {
      if (fstat(c->source, &src))
        rc = -errno;
      else {
        c->header.source_dev = (uint64_t)src.st_dev;
        c->header.source_ino = (uint64_t)src.st_ino;
        if (src.st_dev != dst.st_dev)
          rc = -EXDEV;
        else if (src.st_ino == dst.st_ino)
          rc = -EINVAL;
      }
    }
  }
  return rc;
}
static int context_close(merge_context_t *c, int rc) {
  if (c->source >= 0)
    rc = closed(c->source, rc);
  if (c->destination >= 0)
    rc = closed(c->destination, rc);
  return rc;
}
static FILE *manifest_open(const char *path, int *error) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0) {
    *error = -errno;
    return NULL;
  }
  struct stat st;
  int rc = fstat(fd, &st) ? -errno : 0;
  if (!rc &&
      (!S_ISREG(st.st_mode) || (st.st_mode & 077) ||
       (st.st_uid != geteuid() && st.st_uid != 0) ||
       st.st_size < HEADER_SIZE + 4 ||
       (uint64_t)st.st_size >
           HEADER_SIZE + 4ULL + BFMERGE_MAX_ENTRIES * (uint64_t)ENTRY_MAX))
    rc = -EACCES;
  if (rc) {
    close(fd);
    *error = rc;
    return NULL;
  }
  FILE *f = fdopen(fd, "rb");
  if (!f) {
    *error = -errno;
    close(fd);
  }
  return f;
}
int bfpilot_archive_merge(const char *stage, const char *destination,
                          const char *id, const char *manifest,
                          const bfpilot_merge_options_t *options,
                          bfpilot_merge_result_t *result) {
  if (!result)
    return -EINVAL;
  memset(result, 0, sizeof(*result));
  merge_context_t c;
  int rc = context_open(&c, stage, destination, id, options, result, manifest);
  if (rc)
    return context_close(&c, rc);
  int opened = 0;
  FILE *f = manifest_open(manifest, &opened);
  if (!f && opened == -ENOENT) {
    rc = prepare_manifest(&c, manifest);
    if (!rc)
      f = manifest_open(manifest, &rc);
  } else if (!f)
    rc = opened;
  if (f) {
    rc = read_manifest(f, &c, 1);
    if (!rc && fseek(f, HEADER_SIZE, SEEK_SET))
      rc = -(errno ? errno : EIO);
    result->entries = c.header.entries;
    result->files = c.header.files;
    result->bytes = c.header.bytes;
    for (uint32_t i = 0; !rc && i < c.header.entries; i++) {
      if (cancelled(&c)) {
        rc = -ECANCELED;
        break;
      }
      merge_entry_t e;
      rc = record_read(f, &e, NULL);
      if (!rc)
        rc = publish_entry(&c, &e);
      if (!rc) {
        result->published++;
        if (options && options->progress)
          options->progress(options->context, e.relative, result->published,
                            result->entries);
      }
    }
    rc = stream_closed(f, rc);
  }
  if (!rc && c.source >= 0)
    rc = bfpilot_tree_walk_post(stage, (dev_t)c.header.source_dev,
                                remove_empty_directory, NULL, NULL);
  if (!rc)
    result->error_path[0] = 0;
  return context_close(&c, rc);
}
int bfpilot_archive_merge_forget(const char *stage, const char *destination,
                                 const char *id, const char *manifest) {
  bfpilot_merge_result_t result = {0};
  merge_context_t c;
  int rc = context_open(&c, stage, destination, id, NULL, &result, manifest);
  if (rc)
    return context_close(&c, rc);
  if (c.source >= 0)
    return context_close(&c, -EBUSY);
  FILE *f = manifest_open(manifest, &rc);
  if (f) {
    rc = read_manifest(f, &c, 1);
    rc = stream_closed(f, rc);
  }
  if (!rc && unlink(manifest))
    rc = -errno;
  return context_close(&c, rc);
}
