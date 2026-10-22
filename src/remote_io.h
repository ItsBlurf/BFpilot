#ifndef BFPILOT_REMOTE_IO_H
#define BFPILOT_REMOTE_IO_H
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
typedef struct bfpilot_io bfpilot_io_t;
typedef struct bfpilot_io_file bfpilot_io_file_t;
typedef struct bfpilot_io_dir bfpilot_io_dir_t;
/* One worker owns each endpoint and its handles. All failures are -errno. */
int bfpilot_io_endpoint(const char *path, bfpilot_io_t **out);
void bfpilot_io_end(bfpilot_io_t *endpoint);
int bfpilot_io_remote(const bfpilot_io_t *endpoint);
const char *bfpilot_io_root(const bfpilot_io_t *endpoint);
typedef struct bfpilot_io_space {
  uint64_t total, free_bytes, available;
} bfpilot_io_space_t;
/* Report server/user-available capacity; unsupported queries remain errors. */
int bfpilot_io_space(bfpilot_io_t *, const char *, bfpilot_io_space_t *);
int bfpilot_io_mkdirs(bfpilot_io_t *, const char *, mode_t);
int bfpilot_io_lstat(bfpilot_io_t *, const char *, struct stat *);
int bfpilot_io_open(bfpilot_io_t *, const char *, int flags, mode_t,
                    bfpilot_io_file_t **);
ssize_t bfpilot_io_read(bfpilot_io_file_t *, void *, size_t);
ssize_t bfpilot_io_write(bfpilot_io_file_t *, const void *, size_t);
int bfpilot_io_seek(bfpilot_io_file_t *, off_t offset);
int bfpilot_io_fstat(bfpilot_io_file_t *, struct stat *);
int bfpilot_io_sync(bfpilot_io_file_t *);
int bfpilot_io_chmod_file(bfpilot_io_file_t *,mode_t);
int bfpilot_io_close(bfpilot_io_file_t *);
int bfpilot_io_mkdir(bfpilot_io_t *, const char *, mode_t);
int bfpilot_io_remove(bfpilot_io_t *, const char *, int directory);
int bfpilot_io_rename(bfpilot_io_t *, const char *, const char *, int replace);
int bfpilot_io_opendir(bfpilot_io_t *, const char *, bfpilot_io_dir_t **);
/* 1 for a child, 0 for end, negative errno for failure. Never returns . or ...
 */
int bfpilot_io_readdir(bfpilot_io_dir_t *, char *name, size_t, struct stat *);
int bfpilot_io_closedir(bfpilot_io_dir_t *);
int bfpilot_io_walk(bfpilot_io_t *, const char *root, int postorder,
                    int (*entry)(const char *, const char *,
                                 const struct stat *, void *),
                    int (*cancelled)(void *), void *context);
#endif
