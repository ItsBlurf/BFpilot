#ifndef BFPILOT_FS_AT_H
#define BFPILOT_FS_AT_H
#include <sys/stat.h>
#ifdef __cplusplus
extern "C" {
#endif
/* POSIX error semantics, including SDK fallbacks that expose raw errno in RAX. */
int bfpilot_fs_openat(int dir, const char *path, int flags, mode_t mode);
int bfpilot_fs_mkdirat(int dir, const char *path, mode_t mode);
int bfpilot_fs_fstatat(int dir, const char *path, struct stat *st, int flags);
int bfpilot_fs_unlinkat(int dir, const char *path, int flags);
int bfpilot_fs_renameat(int old_dir, const char *old_path,
                       int new_dir, const char *new_path);
#ifdef __cplusplus
}
#endif
#endif
