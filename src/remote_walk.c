/* Iterative mixed-filesystem walk. A server may resolve native symlinks before
 * reporting SMB metadata: visited identities and depth remain mandatory. */
#include "remote_io.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WALK_DEPTH 64U
#define WALK_DIRS 262144U
typedef struct {
  dev_t device;
  ino_t inode;
  int used;
} identity;
typedef struct {
  identity *slots;
  size_t capacity, count;
} visited;
typedef struct {
  char path[1024], relative[1024];
  struct stat st;
  bfpilot_io_dir_t *directory;
} frame;
static size_t slot(dev_t device, ino_t inode, size_t capacity) {
  return ((uint64_t)device * UINT64_C(0x9e3779b97f4a7c15) ^
          (uint64_t)inode * UINT64_C(0xbf58476d1ce4e5b9)) &
         (capacity - 1);
}
static int admit(visited *set, const struct stat *st) {
  if (set->count == WALK_DIRS)
    return -E2BIG;
  /* Some servers omit file IDs. Keep their walk bounded without treating all
   * such directories as the same object. Depth is still an independent gate. */
  if (!st->st_ino) {
    set->count++;
    return 0;
  }
  if (!set->capacity || set->count * 2 >= set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 128;
    identity *slots = calloc(capacity, sizeof(*slots));
    if (!slots)
      return -ENOMEM;
    for (size_t i = 0; i < set->capacity; i++)
      if (set->slots[i].used) {
        size_t n = slot(set->slots[i].device, set->slots[i].inode, capacity);
        while (slots[n].used)
          n = (n + 1) & (capacity - 1);
        slots[n] = set->slots[i];
      }
    free(set->slots);
    set->slots = slots;
    set->capacity = capacity;
  }
  size_t n = slot(st->st_dev, st->st_ino, set->capacity);
  while (set->slots[n].used) {
    if (set->slots[n].device == st->st_dev && set->slots[n].inode == st->st_ino)
      return -ELOOP;
    n = (n + 1) & (set->capacity - 1);
  }
  set->slots[n] = (identity){st->st_dev, st->st_ino, 1};
  set->count++;
  return 0;
}
static int joined(char *out, size_t capacity, const char *parent,
                  const char *name) {
  size_t left = strlen(parent), right = strlen(name),
         slash = left && parent[left - 1] != '/';
  if (left + slash + right >= capacity)
    return -ENAMETOOLONG;
  memcpy(out, parent, left);
  if (slash)
    out[left++] = '/';
  memcpy(out + left, name, right + 1);
  return 0;
}
int bfpilot_io_walk(bfpilot_io_t *endpoint, const char *root, int postorder,
                    int (*entry)(const char *, const char *,
                                 const struct stat *, void *),
                    int (*cancelled)(void *), void *context) {
  if (!endpoint || !root || !entry || strlen(root) >= 1024)
    return -EINVAL;
  struct stat first;
  int rc = bfpilot_io_lstat(endpoint, root, &first);
  if (rc)
    return rc;
  if (cancelled && cancelled(context))
    return -ECANCELED;
  if (S_ISREG(first.st_mode))
    return entry(root, "", &first, context);
  if (!S_ISDIR(first.st_mode))
    return -ENOTSUP;
  // One scratch child frame also serves a file below the deepest directory.
  frame *frames = calloc(WALK_DEPTH + 2, sizeof(*frames));
  if (!frames)
    return -ENOMEM;
  visited seen = {0};
  size_t depth = 0;
  snprintf(frames[0].path, sizeof(frames[0].path), "%s", root);
  frames[0].st = first;
  rc = admit(&seen, &first);
  if (!rc && !postorder)
    rc = entry(root, "", &first, context);
  if (!rc)
    rc = bfpilot_io_opendir(endpoint, root, &frames[0].directory);
  while (!rc) {
    if (cancelled && cancelled(context)) {
      rc = -ECANCELED;
      break;
    }
    frame *current = frames + depth;
    char name[256];
    struct stat st;
    int got = bfpilot_io_readdir(current->directory, name, sizeof(name), &st);
    if (got < 0) {
      rc = got;
      break;
    }
    if (!got) {
      rc = bfpilot_io_closedir(current->directory);
      current->directory = NULL;
      if (!rc && postorder)
        rc = entry(current->path, current->relative, &current->st, context);
      if (rc || !depth)
        break;
      depth--;
      continue;
    }
    if (st.st_dev != first.st_dev) {
      rc = -EXDEV;
      break;
    }
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
      rc = -ENOTSUP;
      break;
    }
    if (depth == WALK_DEPTH && S_ISDIR(st.st_mode)) {
      rc = -ELOOP;
      break;
    }
    frame *child = frames + depth + 1;
    rc = joined(child->path, sizeof(child->path), current->path, name);
    if (!rc)
      rc = joined(child->relative, sizeof(child->relative), current->relative,
                  name);
    if (rc)
      break;
    child->st = st;
    if (S_ISDIR(st.st_mode)) {
      rc = admit(&seen, &st);
      if (!rc && !postorder)
        rc = entry(child->path, child->relative, &st, context);
      if (!rc)
        rc = bfpilot_io_opendir(endpoint, child->path, &child->directory);
      if (!rc)
        depth++;
    } else
      rc = entry(child->path, child->relative, &st, context);
  }
  for (size_t i = 0; i <= WALK_DEPTH + 1; i++)
    if (frames[i].directory) {
      int closed = bfpilot_io_closedir(frames[i].directory);
      if (!rc)
        rc = closed;
    }
  free(seen.slots);
  free(frames);
  return rc;
}
