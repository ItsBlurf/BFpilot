/* A private bounded source registry outlives a file-manager main process.
 * Records are committed before their capability URL can be returned. */
#include "pkg_source_store.h"
#include "diag.h"
#include "fs_at.h"
#include "paths.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#ifndef SOURCE_DIRECTORY
#define SOURCE_DIRECTORY BFPILOT_DATA_DIR "/pkg-sources"
#endif
#define SOURCE_IDLE_SECONDS (24ULL * 60 * 60)
#define SOURCE_TOUCH_SECONDS (10ULL * 60)
#define SOURCE_JOURNAL_MAX (56U + BFPILOT_PKG_SOURCE_REGISTER_MAX)
typedef struct source_entry {
  int used, retired;
  unsigned refs;
  uint64_t seen, persisted;
  char token[33], path[BFPILOT_PKG_SOURCE_PATH_SIZE];
  bfpilot_pkg_source_stamp_t stamp;
} source_entry_t;
static source_entry_t entries[BFPILOT_PKG_SOURCE_MAX_RECORDS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int directory = -1;
static uint64_t now_seconds(void) {
  time_t value = time(NULL);
  return value < 0 ? 0 : (uint64_t)value;
}
static int exact_read(int fd, void *data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = read(fd, (unsigned char *)data + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return n < 0 ? -errno : -EIO;
    done += (size_t)n;
  }
  return 0;
}
static int exact_write(int fd, const void *data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = write(fd, (const unsigned char *)data + done, size - done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return n < 0 ? -errno : -EIO;
    done += (size_t)n;
  }
  return 0;
}
static int save_entry(source_entry_t *entry) {
  unsigned char encoded[SOURCE_JOURNAL_MAX];
  size_t size;
  int rc = bfps_journal_encode(entry->token, entry->seen, entry->path,
                               &entry->stamp, encoded, sizeof(encoded), &size);
  if (rc)
    return rc;
  char temporary[80], name[40];
  snprintf(temporary, sizeof(temporary), ".tmp-%ld-%s", (long)getpid(),
           entry->token);
  snprintf(name, sizeof(name), "%s.src", entry->token);
  int fd = bfpilot_fs_openat(directory, temporary,
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -errno;
  rc = exact_write(fd, encoded, size);
  if (!rc && fsync(fd))
    rc = -errno;
  if (close(fd) && !rc)
    rc = -errno;
  if (!rc && bfpilot_fs_renameat(directory, temporary, directory, name))
    rc = -errno;
  if (rc) {
    int saved = rc;
    (void)bfpilot_fs_unlinkat(directory, temporary, 0);
    return saved;
  }
  entry->persisted = entry->seen;
  return 0;
}
static int delete_entry(source_entry_t *entry) {
  char name[40];
  snprintf(name, sizeof(name), "%s.src", entry->token);
  if (bfpilot_fs_unlinkat(directory, name, 0) && errno != ENOENT)
    return -errno;
  entry->retired = 1;
  if (!entry->refs)
    memset(entry, 0, sizeof(*entry));
  return 0;
}
static int expired(const source_entry_t *entry, uint64_t now) {
  return entry->used && !entry->retired && !entry->refs && now >= entry->seen &&
         now - entry->seen > SOURCE_IDLE_SECONDS;
}
static int secure_random(unsigned char data[16]) {
  /* Unlike an unchecked PRNG seed fallback, report entropy-source failure. */
  int mib[2] = {CTL_KERN, KERN_ARND};
  size_t size = 16;
  if (!sysctl(mib, 2, data, &size, NULL, 0) && size == 16)
    return 0;
  int fd = open("/dev/urandom", O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    return -errno;
  int rc = exact_read(fd, data, 16);
  if (close(fd) && !rc)
    rc = -errno;
  return rc;
}
int bfps_store_init(void) {
  struct stat st;
  if (mkdir(SOURCE_DIRECTORY, 0700) && errno != EEXIST)
    return -errno;
  if (lstat(SOURCE_DIRECTORY, &st))
    return -errno;
  if (!S_ISDIR(st.st_mode) || (st.st_mode & 077) ||
      (st.st_uid != geteuid() && st.st_uid != 0))
    return -EACCES;
  directory = open(SOURCE_DIRECTORY, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory < 0)
    return -errno;
  struct stat opened;
  if (fstat(directory, &opened))
    return -errno;
  if (opened.st_dev != st.st_dev || opened.st_ino != st.st_ino)
    return -ESTALE;
  int duplicate = dup(directory);
  if (duplicate < 0)
    return -errno;
  DIR *dir = fdopendir(duplicate);
  if (!dir) {
    int rc = -errno;
    close(duplicate);
    return rc;
  }
  unsigned count = 0, scanned = 0;
  int rc = 0;
  for (;;) {
    errno = 0;
    struct dirent *item = readdir(dir);
    if (!item) {
      if (errno)
        rc = -errno;
      break;
    }
    if (++scanned > BFPILOT_PKG_SOURCE_MAX_RECORDS * 4U) {
      rc = -E2BIG;
      break;
    }
    size_t length = strlen(item->d_name);
    if (length != 36 || strcmp(item->d_name + 32, ".src") ||
        !bfps_token_valid(item->d_name, 32))
      continue;
    if (count == BFPILOT_PKG_SOURCE_MAX_RECORDS) {
      rc = -E2BIG;
      break;
    }
    int fd =
        bfpilot_fs_openat(directory, item->d_name, O_RDONLY | O_NOFOLLOW, 0);
    if (fd < 0) {
      bfpilot_log("pkg source: cannot read retained record errno=%d", errno);
      continue;
    }
    struct stat file;
    unsigned char data[SOURCE_JOURNAL_MAX];
    source_entry_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    int loaded = fstat(fd, &file) ? -errno : 0;
    if (!loaded && (!S_ISREG(file.st_mode) || (file.st_mode & 077) ||
                    file.st_size < 0 || (uint64_t)file.st_size > sizeof(data) ||
                    (file.st_uid != geteuid() && file.st_uid != 0)))
      loaded = -EINVAL;
    if (!loaded)
      loaded = exact_read(fd, data, (size_t)file.st_size);
    if (close(fd) && !loaded)
      loaded = -errno;
    if (!loaded)
      loaded = bfps_journal_decode(data, (size_t)file.st_size, candidate.token,
                                   &candidate.seen, candidate.path,
                                   sizeof(candidate.path), &candidate.stamp);
    if (!loaded && memcmp(candidate.token, item->d_name, 32))
      loaded = -EINVAL;
    if (loaded) {
      bfpilot_log(
          "pkg source: invalid retained record kept for inspection rc=%d",
          loaded);
      continue;
    }
    candidate.used = 1;
    candidate.persisted = candidate.seen;
    if (expired(&candidate, now_seconds())) {
      int removed = delete_entry(&candidate);
      if (!removed)
        continue;
      bfpilot_log("pkg source: expired retained record cleanup failed rc=%d",
                  removed);
    }
    entries[count++] = candidate;
  }
  if (closedir(dir) && !rc)
    rc = -errno;
  return rc;
}
int bfps_store_register(const char *path,
                        const bfpilot_pkg_source_stamp_t *stamp,
                        char token[33]) {
  if (!token)
    return -EINVAL;
  unsigned char validation[BFPILOT_PKG_SOURCE_REGISTER_MAX];
  size_t size;
  int rc =
      bfps_register_encode(path, stamp, validation, sizeof(validation), &size);
  if (rc)
    return rc;
  pthread_mutex_lock(&lock);
  size_t slot = 0;
  for (; slot < BFPILOT_PKG_SOURCE_MAX_RECORDS; slot++)
    if (!entries[slot].used)
      break;
  if (slot == BFPILOT_PKG_SOURCE_MAX_RECORDS) {
    pthread_mutex_unlock(&lock);
    return -ENOSPC;
  }
  source_entry_t *entry = &entries[slot];
  memset(entry, 0, sizeof(*entry));
  for (unsigned attempt = 0; attempt < 8; attempt++) {
    unsigned char random[16];
    rc = secure_random(random);
    if (rc)
      break;
    for (unsigned i = 0; i < 16; i++)
      snprintf(entry->token + i * 2, 3, "%02x", random[i]);
    int duplicate = 0;
    for (size_t i = 0; i < BFPILOT_PKG_SOURCE_MAX_RECORDS; i++)
      if (entries[i].used && !strcmp(entries[i].token, entry->token)) {
        duplicate = 1;
        break;
      }
    char name[40];
    snprintf(name, sizeof(name), "%s.src", entry->token);
    struct stat old;
    if (!bfpilot_fs_fstatat(directory, name, &old, AT_SYMLINK_NOFOLLOW))
      duplicate = 1;
    else if (errno != ENOENT) {
      rc = -errno;
      break;
    }
    if (duplicate) {
      rc = -EEXIST;
      continue;
    }
    entry->stamp = *stamp;
    snprintf(entry->path, sizeof(entry->path), "%s", path);
    entry->seen = now_seconds();
    rc = save_entry(entry);
    if (rc)
      break;
    entry->used = 1;
    memcpy(token, entry->token, 33);
    break;
  }
  if (rc)
    memset(entry, 0, sizeof(*entry));
  pthread_mutex_unlock(&lock);
  return rc;
}
int bfps_store_drop(const char *token) {
  if (!token || !bfps_token_valid(token, strlen(token)))
    return -EINVAL;
  int rc = -ENOENT;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < BFPILOT_PKG_SOURCE_MAX_RECORDS; i++)
    if (entries[i].used && !entries[i].retired &&
        !strcmp(entries[i].token, token)) {
      rc = delete_entry(&entries[i]);
      break;
    }
  pthread_mutex_unlock(&lock);
  return rc;
}
int bfps_store_acquire(const char *token, bfps_source_handle_t *handle) {
  if (!handle || !token || !bfps_token_valid(token, strlen(token)))
    return -EINVAL;
  int rc = -ENOENT;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < BFPILOT_PKG_SOURCE_MAX_RECORDS; i++) {
    source_entry_t *entry = &entries[i];
    if (!entry->used || entry->retired || strcmp(entry->token, token))
      continue;
    if (expired(entry, now_seconds())) {
      int removed = delete_entry(entry);
      rc = removed ? removed : -ENOENT;
      break;
    }
    if (entry->refs == UINT_MAX) {
      rc = -EBUSY;
      break;
    }
    entry->refs++;
    entry->seen = now_seconds();
    handle->slot = i;
    memcpy(handle->token, entry->token, 33);
    memcpy(handle->path, entry->path, sizeof(handle->path));
    handle->stamp = entry->stamp;
    rc = 0;
    break;
  }
  pthread_mutex_unlock(&lock);
  return rc;
}
void bfps_store_release(const bfps_source_handle_t *handle) {
  if (!handle || handle->slot >= BFPILOT_PKG_SOURCE_MAX_RECORDS)
    return;
  pthread_mutex_lock(&lock);
  source_entry_t *entry = &entries[handle->slot];
  if (entry->used && entry->refs && !strcmp(entry->token, handle->token)) {
    entry->refs--;
    if (entry->retired && !entry->refs)
      memset(entry, 0, sizeof(*entry));
  }
  pthread_mutex_unlock(&lock);
}
void bfps_store_touch(const bfps_source_handle_t *handle) {
  if (!handle || handle->slot >= BFPILOT_PKG_SOURCE_MAX_RECORDS)
    return;
  pthread_mutex_lock(&lock);
  source_entry_t *entry = &entries[handle->slot];
  if (entry->used && !entry->retired && entry->refs &&
      !strcmp(entry->token, handle->token)) {
    entry->seen = now_seconds();
    if (entry->seen >= entry->persisted &&
        entry->seen - entry->persisted >= SOURCE_TOUCH_SECONDS) {
      int rc = save_entry(entry);
      if (rc)
        bfpilot_log("pkg source: activity journal update failed rc=%d", rc);
    }
  }
  pthread_mutex_unlock(&lock);
}
void bfps_store_expire(void) {
  uint64_t now = now_seconds();
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < BFPILOT_PKG_SOURCE_MAX_RECORDS; i++)
    if (expired(&entries[i], now)) {
      int rc = delete_entry(&entries[i]);
      if (rc)
        bfpilot_log("pkg source: idle source cleanup failed rc=%d", rc);
    }
  pthread_mutex_unlock(&lock);
}
void bfps_store_stats(unsigned *records, unsigned *streams) {
  unsigned n = 0, refs = 0;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < BFPILOT_PKG_SOURCE_MAX_RECORDS; i++)
    if (entries[i].used) {
      n++;
      refs += entries[i].refs;
    }
  pthread_mutex_unlock(&lock);
  if (records)
    *records = n;
  if (streams)
    *streams = refs;
}
