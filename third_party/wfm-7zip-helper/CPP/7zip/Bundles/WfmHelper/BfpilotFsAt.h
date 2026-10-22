/* BFpilot SDK adapter. Original WFM/7-Zip files retain their own licenses. */
#ifndef BFPILOT_HELPER_FS_AT_H
#define BFPILOT_HELPER_FS_AT_H
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../../../../src/fs_at.h"
static inline int BfpilotHelperOpenAt(int dir, const char *path, int flags,
                                     mode_t mode = 0) {
  return bfpilot_fs_openat(dir, path, flags, mode);
}
#define openat BfpilotHelperOpenAt
#define fstatat bfpilot_fs_fstatat
#define mkdirat bfpilot_fs_mkdirat
#define unlinkat bfpilot_fs_unlinkat
#define renameat bfpilot_fs_renameat
#endif
