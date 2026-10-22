#include "fs_at.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#ifdef __PROSPERO__
#include <sys/syscall.h>
/* The installed SDK libc's at-syscall fallbacks return directly after syscall,
 * leaving a positive errno in RAX on carry. That value is indistinguishable
 * from a valid fd for openat. Capture carry at the syscall boundary instead of
 * interpreting a positive result or a stale thread errno afterwards. */
static long syscall_at(long number, long a, long b, long c, long d) {
  register long fourth __asm__("r10") = d;
  long result;
  unsigned char failed;
  __asm__ volatile("syscall; setc %1"
      : "=a"(result), "=qm"(failed)
      : "a"(number), "D"(a), "S"(b), "d"(c), "r"(fourth)
      : "rcx", "r11", "memory", "cc");
  if(failed) { errno = (int)result; return -1; }
  return result;
}
#endif
int bfpilot_fs_openat(int dir, const char *path, int flags, mode_t mode) {
#ifdef __PROSPERO__
  return (int)syscall_at(SYS_openat, dir, (long)(uintptr_t)path, flags, mode);
#else
  return openat(dir, path, flags, mode);
#endif
}
int bfpilot_fs_mkdirat(int dir, const char *path, mode_t mode) {
#ifdef __PROSPERO__
  return (int)syscall_at(SYS_mkdirat, dir, (long)(uintptr_t)path, mode, 0);
#else
  return mkdirat(dir, path, mode);
#endif
}
int bfpilot_fs_fstatat(int dir, const char *path, struct stat *st, int flags) {
#ifdef __PROSPERO__
  return (int)syscall_at(SYS_fstatat, dir, (long)(uintptr_t)path, (long)(uintptr_t)st, flags);
#else
  return fstatat(dir, path, st, flags);
#endif
}
