/*
 * BFpilot - archive extraction worker.
 *
 * This file is linked into bfpilot.elf as its integrated archive component.
 */

#include "archive_worker.h"
#include "archive_batch_store.h"
#include "full_archive_client.h"
#include "tree_chmod.h"
#include "tree_walk.h"
#include "staging.h"
#include "rar.hpp"
#include "ps5_7z.hpp"

#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <memory>
#include <string>
#include <sys/cpuset.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern "C" int posix_fallocate(int fd, off_t offset, off_t len);

#ifndef BFPILOT_ARCHIVE_INTEGRATED
#define BFPILOT_ARCHIVE_INTEGRATED 0
#endif

#include "paths.h"
#if BFPILOT_ARCHIVE_INTEGRATED
#define BFPILOT_ARCHIVE_DIR BFPILOT_ARCHIVE_INTEGRATED_DIR
#else
#define BFPILOT_ARCHIVE_DIR BFPILOT_ARCHIVE_STANDALONE_DIR
#endif
#define BFPILOT_ARCHIVE_JOB BFPILOT_ARCHIVE_DIR "/job.ini"
#define BFPILOT_ARCHIVE_STATUS BFPILOT_ARCHIVE_DIR "/status.json"
#define BFPILOT_ARCHIVE_STATUS_TMP BFPILOT_ARCHIVE_DIR "/status.tmp"
#define BFPILOT_ARCHIVE_LOG BFPILOT_ARCHIVE_DIR "/archive-worker.log"
#define BFPILOT_ARCHIVE_DAEMON_LOCK BFPILOT_ARCHIVE_DIR "/daemon.lock"
#define BFPILOT_FULL_ACTIVE BFPILOT_ARCHIVE_DIR "/full-active.ini"
#define BFPILOT_FULL_ACTIVE_TMP BFPILOT_ARCHIVE_DIR "/full-active.tmp"

#define ZIP_SIG_LOCAL 0x04034b50U
#define ZIP_SIG_CENTRAL 0x02014b50U
#define ZIP_SIG_END 0x06054b50U
#define ZIP_SIG_ZIP64_END 0x06064b50U
#define ZIP_SIG_ZIP64_LOCATOR 0x07064b50U
#define BFPILOT_ARCHIVE_NICE 4
#define BFPILOT_ARCHIVE_MIN_NICE -20
#define BFPILOT_ARCHIVE_MAX_NICE 20
#define BFPILOT_ZIP_IN_BUFFER_SIZE (16U * 1024U * 1024U)
#define BFPILOT_ZIP_OUT_BUFFER_SIZE (64U * 1024U * 1024U)
#define BFPILOT_ARCHIVE_IO_ALIGNMENT 0x4000U
#define BFPILOT_ARCHIVE_WRITEBACK_SYNC_BYTES (256ULL * 1024ULL * 1024ULL)
#define BFPILOT_ARCHIVE_PREALLOC_MIN_SIZE (64ULL * 1024ULL * 1024ULL)
#define BFPILOT_ARCHIVE_SPACE_MARGIN_BYTES (256ULL * 1024ULL * 1024ULL)
#define BFPILOT_ZIP_MAX_ENTRIES 250000ULL
#define BFPILOT_ZIP_MAX_MEMBER_PATH 1024U
#define BFPILOT_7Z_MAX_ENTRIES 250000U
#define BFPILOT_7Z_BOMB_MIN_UNPACKED_BYTES (256ULL * 1024ULL * 1024ULL)
#define BFPILOT_7Z_MAX_EXPANSION_RATIO 512ULL

struct ArchiveConfig {
  std::string source;
  std::string destination;
  std::string password;
  std::string request_id;
  unsigned threads = 0;
  int priority_nice = 0;
  bool cleanup_partial = false;
};

struct WorkerStatus {
  std::string request_id;
  std::string state;
  std::string archive_type;
  std::string source;
  std::string destination;
  std::string stage;
  std::string current;
  std::string error;
  unsigned long long total_bytes = 0;
  unsigned long long bytes_written = 0;
  unsigned long long files_done = 0;
  unsigned long long total_files = 0;
  unsigned threads = 0;
  unsigned effective_threads = 0;
  bool full_engine = false;
  bool helper_quiescent = true;
  bool helper_task_admitted = false;
  unsigned long long helper_task_id = 0;
  unsigned long long decoder_done = 0, decoder_total = 0;
  int percent = 0;
  int errno_code = 0;
  int archive_exit_code = 0;
  int priority_nice = 0;
  int priority_rc = 0;
  int priority_errno = 0;
  long started_ms = 0;
};

static WorkerStatus g_status;
static bool g_terminal_status_durable = false;
static long g_last_status_ms = 0;
static int g_last_progress = -1;
static std::atomic<unsigned long long> g_io_input_bytes(0);
static std::atomic<unsigned long long> g_io_output_bytes(0);
static std::atomic<unsigned long long> g_io_input_ops(0);
static std::atomic<unsigned long long> g_io_output_ops(0);
static std::atomic<unsigned long long> g_io_input_usec(0);
static std::atomic<unsigned long long> g_io_output_usec(0);
static std::atomic<unsigned long long> g_rar_mt_batches(0);
static std::atomic<unsigned long long> g_rar_mt_threaded_batches(0);
static std::atomic<unsigned long long> g_rar_mt_blocks(0);
static std::atomic<unsigned long long> g_rar_mt_threaded_blocks(0);
static std::atomic<unsigned long long> g_rar_mt_large_blocks(0);
static struct rusage g_usage_start;
static int g_usage_started = 0;

extern "C" int g_archive_cancel = 0;

static long
NowMs(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static unsigned long long
NowUsec(void) {
  struct timespec ts;
  if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (unsigned long long)ts.tv_sec * 1000000ULL +
         (unsigned long long)(ts.tv_nsec / 1000ULL);
}

static void
ResetIoCounters(void) {
  g_io_input_bytes.store(0);
  g_io_output_bytes.store(0);
  g_io_input_ops.store(0);
  g_io_output_ops.store(0);
  g_io_input_usec.store(0);
  g_io_output_usec.store(0);
  g_rar_mt_batches.store(0);
  g_rar_mt_threaded_batches.store(0);
  g_rar_mt_blocks.store(0);
  g_rar_mt_threaded_blocks.store(0);
  g_rar_mt_large_blocks.store(0);
  memset(&g_usage_start, 0, sizeof(g_usage_start));
  g_usage_started = getrusage(RUSAGE_SELF, &g_usage_start) == 0 ? 1 : 0;
}

extern "C" void
Ps5ArchiveInputSample(unsigned long long bytes, unsigned long long usec) {
  if(bytes == 0) return;
  g_io_input_bytes.fetch_add(bytes);
  g_io_input_ops.fetch_add(1);
  if(usec > 0) g_io_input_usec.fetch_add(usec);
}

extern "C" void
Ps5ArchiveOutputSample(unsigned long long bytes, unsigned long long usec) {
  if(bytes == 0) return;
  g_io_output_bytes.fetch_add(bytes);
  g_io_output_ops.fetch_add(1);
  if(usec > 0) g_io_output_usec.fetch_add(usec);
}

extern "C" void
Ps5ArchiveRarMtSample(unsigned blocks, unsigned threaded_blocks,
                      unsigned large_blocks, unsigned threaded_batch) {
  if(blocks == 0) return;
  g_rar_mt_batches.fetch_add(1);
  g_rar_mt_blocks.fetch_add(blocks);
  if(threaded_blocks > 0) {
    g_rar_mt_threaded_blocks.fetch_add(threaded_blocks);
  }
  if(large_blocks > 0) {
    g_rar_mt_large_blocks.fetch_add(large_blocks);
  }
  if(threaded_batch) {
    g_rar_mt_threaded_batches.fetch_add(1);
  }
}

static std::string
Trim(const std::string &value) {
  size_t start = 0;
  while(start < value.size() && (unsigned char)value[start] <= ' ') start++;
  size_t end = value.size();
  while(end > start && (unsigned char)value[end - 1] <= ' ') end--;
  return value.substr(start, end - start);
}

static std::string
ToLowerAscii(const std::string &value) {
  std::string out = value;
  for(size_t i = 0; i < out.size(); i++) {
    if(out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
  }
  return out;
}

static bool
EndsWithNoCase(const std::string &value, const std::string &suffix) {
  if(value.size() < suffix.size()) return false;
  return ToLowerAscii(value.substr(value.size() - suffix.size())) ==
         ToLowerAscii(suffix);
}

static std::string
BaseName(const std::string &path) {
  size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

static std::string
DirName(const std::string &path) {
  size_t pos = path.find_last_of('/');
  if(pos == std::string::npos) return ".";
  if(pos == 0) return "/";
  return path.substr(0, pos);
}

static std::string
JoinPath(const std::string &left, const std::string &right) {
  if(left.empty() || right.empty() || right[0] == '/') return right;
  if(left[left.size() - 1] == '/') return left + right;
  return left + "/" + right;
}

static bool
MkdirAll(const std::string &path) {
  if(path.empty() || path == "/") return true;

  std::string cur;
  size_t pos = path[0] == '/' ? 1 : 0;
  if(path[0] == '/') cur = "/";

  while(pos <= path.size()) {
    size_t next = path.find('/', pos);
    std::string part =
        path.substr(pos, next == std::string::npos ? std::string::npos
                                                   : next - pos);
    if(!part.empty()) {
      if(!cur.empty() && cur[cur.size() - 1] != '/') cur += "/";
      cur += part;
      if(mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
    }
    if(next == std::string::npos) break;
    pos = next + 1;
  }
  return true;
}

static bool
PathExists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static bool
IsDirPath(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool
HasDotDotSegment(const std::string &path) {
  size_t pos = 0;
  while(pos <= path.size()) {
    size_t next = path.find('/', pos);
    std::string part =
        path.substr(pos, next == std::string::npos ? std::string::npos
                                                   : next - pos);
    if(part == "..") return true;
    if(next == std::string::npos) break;
    pos = next + 1;
  }
  return false;
}

static bool
StartsWithRoot(const std::string &path, const char *root) {
  size_t n = strlen(root);
  return path.compare(0, n, root) == 0 &&
         (path.size() == n || path[n] == '/');
}

static bool
IsAllowedArchivePath(const std::string &path) {
  if(path.empty() || path[0] != '/' || HasDotDotSegment(path)) return false;
  if(StartsWithRoot(path, "/data")) return true;
  for(int i = 0; i < 8; i++) {
    char root[32];
    snprintf(root, sizeof(root), "/mnt/usb%d", i);
    if(StartsWithRoot(path, root)) return true;
    snprintf(root, sizeof(root), "/mnt/ext%d", i);
    if(StartsWithRoot(path, root)) return true;
  }
  return false;
}

static bool g_stage_owned = false;
static bool g_full_marker_pending = false;
static int RemoveStageEntry(const char *path, const char *, const struct stat *expected, void *) {
  struct stat st;
  if(lstat(path, &st) != 0) return -errno;
  if(st.st_dev != expected->st_dev || st.st_ino != expected->st_ino ||
     (st.st_mode & S_IFMT) != (expected->st_mode & S_IFMT)) return -EAGAIN;
  if((S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path)) != 0) return -errno;
  return 0;
}
static bool DeleteRecursiveForce(const std::string &path) {
  struct stat st;
  if(lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  int rc = bfpilot_tree_walk_post(path.c_str(), st.st_dev, RemoveStageEntry, NULL, NULL);
  if(rc) errno = -rc;
  return rc == 0;
}

static bool
IsSafeMemberPath(const std::string &path) {
  if(path.empty() || path[0] == '/' || path[0] == '\\') return false;
  if(path.size() >= 2 && path[1] == ':') return false;

  size_t len = path.size();
  if(path[len - 1] == '/' || path[len - 1] == '\\') len--;
  if(len == 0) return false;

  size_t pos = 0;
  while(pos < len) {
    size_t next = path.find_first_of("/\\", pos);
    if(next == std::string::npos || next > len) next = len;
    std::string part = path.substr(pos, next - pos);
    if(part.empty() || part == "." || part == "..") return false;
    if(next >= len) break;
    pos = next + 1;
  }
  return true;
}

static std::string
NormalizeMemberPath(const std::string &path) {
  std::string out = path;
  for(size_t i = 0; i < out.size(); i++) {
    if(out[i] == '\\') out[i] = '/';
  }
  return out;
}

static std::string
JsonEscape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for(size_t i = 0; i < value.size(); i++) {
    unsigned char ch = (unsigned char)value[i];
    switch(ch) {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if(ch < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", ch);
        out += buf;
      } else if(ch >= 0x80) {
        char buf[16];
        snprintf(buf, sizeof(buf), "\\udb80\\udc%02x", ch);
        out += buf;
      } else {
        out += (char)ch;
      }
      break;
    }
  }
  return out;
}

static bool
WriteAll(int fd, const void *data, size_t size) {
  const char *p = (const char *)data;
  while(size > 0) {
    ssize_t n = write(fd, p, size);
    if(n < 0) {
      if(errno == EINTR) continue;
      return false;
    }
    if(n == 0) {
      errno = ENOSPC;
      return false;
    }
    p += n;
    size -= (size_t)n;
  }
  return true;
}

static bool
ReadExact(int fd, void *data, size_t size) {
  char *p = (char *)data;
  while(size > 0) {
    ssize_t n = read(fd, p, size);
    if(n < 0) {
      if(errno == EINTR) continue;
      return false;
    }
    if(n == 0) return false;
    p += n;
    size -= (size_t)n;
  }
  return true;
}

static bool
ReadExactAt(int fd, unsigned long long offset, void *data, size_t size) {
  if(lseek(fd, (off_t)offset, SEEK_SET) < 0) return false;
  return ReadExact(fd, data, size);
}

static unsigned
NormalizeArchiveThreads(unsigned requested) {
  if(requested == 0) return 0;
  if(requested > BFPILOT_ARCHIVE_MAX_THREADS) return BFPILOT_ARCHIVE_MAX_THREADS;
  return requested;
}

static unsigned
AutoArchiveThreads(void) {
  long cpu = sysconf(_SC_NPROCESSORS_ONLN);
  if(cpu < 1) cpu = sysconf(_SC_NPROCESSORS_CONF);
  if(cpu < 1) return 1;
  unsigned threads = (unsigned)cpu;
  if(threads > BFPILOT_ARCHIVE_AUTO_MAX_THREADS) {
    threads = BFPILOT_ARCHIVE_AUTO_MAX_THREADS;
  }
  return NormalizeArchiveThreads(threads);
}

static unsigned
EffectiveArchiveThreads(unsigned requested) {
  if(requested > 0) return NormalizeArchiveThreads(requested);
  unsigned threads = AutoArchiveThreads();
  return threads > 0 ? threads : 1;
}

static unsigned
ParseArchiveThreads(const std::string &value) {
  if(value.empty()) return 0;
  for(size_t i = 0; i < value.size(); i++) {
    if(value[i] < '0' || value[i] > '9') return 0;
  }
  return NormalizeArchiveThreads((unsigned)strtoul(value.c_str(), NULL, 10));
}

static int
ParseArchiveNice(const std::string &value) {
  if(value.empty()) return BFPILOT_ARCHIVE_NICE;
  char *end = NULL;
  errno = 0;
  long parsed = strtol(value.c_str(), &end, 10);
  if(errno != 0 || end == value.c_str() || (end && *end)) {
    return BFPILOT_ARCHIVE_NICE;
  }
  if(parsed < BFPILOT_ARCHIVE_MIN_NICE) parsed = BFPILOT_ARCHIVE_MIN_NICE;
  if(parsed > BFPILOT_ARCHIVE_MAX_NICE) parsed = BFPILOT_ARCHIVE_MAX_NICE;
  return (int)parsed;
}

static void
LogLine(const char *fmt, ...) {
  if(!MkdirAll(BFPILOT_ARCHIVE_DIR)) return;

  int fd = open(BFPILOT_ARCHIVE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if(fd < 0) return;

  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  if(n > 0) {
    size_t len = (size_t)std::min(n, (int)sizeof(msg) - 2);
    msg[len++] = '\n';
    WriteAll(fd, msg, len);
  }
  close(fd);
}

static bool
PreallocateOutputFile(int fd, unsigned long long size, const char *context,
                      std::string &error) {
  if(size < BFPILOT_ARCHIVE_PREALLOC_MIN_SIZE ||
     size > (unsigned long long)LLONG_MAX) {
    return true;
  }

  int rc = posix_fallocate(fd, 0, (off_t)size);
  if(rc == 0) {
    LogLine("archive prealloc ok context=%s size=%llu",
            context ? context : "output", size);
    return true;
  }
  if(rc != EOPNOTSUPP && rc != ENOSYS && rc != EINVAL) {
    errno = rc;
    error = std::string("failed to preallocate archive output: ") + strerror(rc);
    LogLine("archive prealloc failed context=%s rc=%d size=%llu",
            context ? context : "output", rc, size);
    return false;
  }

  LogLine("archive prealloc ignored context=%s rc=%d size=%llu",
          context ? context : "output", rc, size);
  return true;
}

static bool
CheckDestinationSpace(const std::string &path, unsigned long long required,
                      const char *context, std::string &error) {
  if(required == 0) return true;

  struct statvfs vfs;
  if(statvfs(path.c_str(), &vfs) != 0) {
    int saved_errno = errno;
    error = "failed to inspect destination free space";
    LogLine("archive space check failed context=%s path=%s errno=%d",
            context ? context : "output", path.c_str(), saved_errno);
    errno = saved_errno;
    return false;
  }

  unsigned long long block =
      vfs.f_frsize ? (unsigned long long)vfs.f_frsize
                   : (unsigned long long)vfs.f_bsize;
  unsigned long long available =
      block != 0 && vfs.f_bavail > ULLONG_MAX / block
          ? ULLONG_MAX
          : (unsigned long long)vfs.f_bavail * block;
  unsigned long long needed =
      required > ULLONG_MAX - BFPILOT_ARCHIVE_SPACE_MARGIN_BYTES
          ? ULLONG_MAX
          : required + BFPILOT_ARCHIVE_SPACE_MARGIN_BYTES;

  if(available < needed) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "not enough free space for %s extraction: need %llu bytes, "
             "available %llu bytes",
             context ? context : "archive", needed, available);
    error = buf;
    LogLine("archive space check failed context=%s path=%s required=%llu "
            "needed=%llu available=%llu",
            context ? context : "output", path.c_str(), required, needed,
            available);
    errno = ENOSPC;
    return false;
  }

  LogLine("archive space check ok context=%s path=%s required=%llu needed=%llu "
          "available=%llu",
          context ? context : "output", path.c_str(), required, needed,
          available);
  return true;
}

static void
ApplyArchiveScheduling(int priority_nice) {
  g_status.priority_nice = priority_nice;
  errno = 0;
  g_status.priority_rc = setpriority(PRIO_PROCESS, 0, priority_nice);
  g_status.priority_errno = g_status.priority_rc == 0 ? 0 : errno;
  LogLine("archive scheduling nice=%d rc=%d errno=%d",
          g_status.priority_nice, g_status.priority_rc,
          g_status.priority_errno);
}

static unsigned long long
TimevalUsec(const struct timeval &tv) {
  return (unsigned long long)tv.tv_sec * 1000000ULL +
         (unsigned long long)tv.tv_usec;
}

static void
WriteStatus(bool force) {
  long now = NowMs();
  if(!force && now > 0 && g_last_status_ms > 0 &&
     now - g_last_status_ms < 750) {
    return;
  }
  g_last_status_ms = now;

  if(!MkdirAll(BFPILOT_ARCHIVE_DIR)) return;

  long elapsed_ms =
      g_status.started_ms > 0 && now > g_status.started_ms
          ? now - g_status.started_ms
          : 0;
  unsigned long long input_bytes = g_io_input_bytes.load();
  unsigned long long output_bytes = g_io_output_bytes.load();
  unsigned long long input_ops = g_io_input_ops.load();
  unsigned long long output_ops = g_io_output_ops.load();
  unsigned long long input_usec = g_io_input_usec.load();
  unsigned long long output_usec = g_io_output_usec.load();
  unsigned long long rate_bytes =
      output_bytes > 0 ? output_bytes:g_status.bytes_written;
  double mbps =
      elapsed_ms > 0
          ? ((double)rate_bytes * 1000.0 / (double)elapsed_ms) /
                (1024.0 * 1024.0)
          : 0.0;
  double input_mbps =
      elapsed_ms > 0
          ? ((double)input_bytes * 1000.0 / (double)elapsed_ms) /
                (1024.0 * 1024.0)
          : 0.0;
  double output_mbps =
      elapsed_ms > 0
          ? ((double)output_bytes * 1000.0 / (double)elapsed_ms) /
                (1024.0 * 1024.0)
          : 0.0;
  double input_wait_mbps =
      input_usec > 0
          ? ((double)input_bytes * 1000000.0 / (double)input_usec) /
                (1024.0 * 1024.0)
          : 0.0;
  double output_wait_mbps =
      output_usec > 0
          ? ((double)output_bytes * 1000000.0 / (double)output_usec) /
                (1024.0 * 1024.0)
          : 0.0;
  unsigned long long cpu_user_ms = 0;
  unsigned long long cpu_system_ms = 0;
  unsigned long long cpu_total_ms = 0;
  double cpu_util_percent = 0.0;
  long voluntary_switches = 0;
  long involuntary_switches = 0;
  long max_rss = 0;
  if(g_usage_started) {
    struct rusage usage_now;
    if(getrusage(RUSAGE_SELF, &usage_now) == 0) {
      unsigned long long start_user = TimevalUsec(g_usage_start.ru_utime);
      unsigned long long now_user = TimevalUsec(usage_now.ru_utime);
      unsigned long long start_system = TimevalUsec(g_usage_start.ru_stime);
      unsigned long long now_system = TimevalUsec(usage_now.ru_stime);
      cpu_user_ms = now_user >= start_user ? (now_user - start_user) / 1000ULL : 0;
      cpu_system_ms =
          now_system >= start_system ? (now_system - start_system) / 1000ULL : 0;
      cpu_total_ms = cpu_user_ms + cpu_system_ms;
      cpu_util_percent = elapsed_ms > 0
                             ? ((double)cpu_total_ms * 100.0) /
                                   (double)elapsed_ms
                             : 0.0;
      voluntary_switches =
          usage_now.ru_nvcsw >= g_usage_start.ru_nvcsw
              ? usage_now.ru_nvcsw - g_usage_start.ru_nvcsw
              : 0;
      involuntary_switches =
          usage_now.ru_nivcsw >= g_usage_start.ru_nivcsw
              ? usage_now.ru_nivcsw - g_usage_start.ru_nivcsw
              : 0;
      max_rss = usage_now.ru_maxrss;
    }
  }
  unsigned long long rar_mt_batches = g_rar_mt_batches.load();
  unsigned long long rar_mt_threaded_batches =
      g_rar_mt_threaded_batches.load();
  unsigned long long rar_mt_blocks = g_rar_mt_blocks.load();
  unsigned long long rar_mt_threaded_blocks =
      g_rar_mt_threaded_blocks.load();
  unsigned long long rar_mt_large_blocks = g_rar_mt_large_blocks.load();

  const std::string escaped[] = {
    JsonEscape(g_status.state), JsonEscape(g_status.archive_type),
    JsonEscape(g_status.source), JsonEscape(g_status.destination),
    JsonEscape(g_status.stage), JsonEscape(g_status.current), JsonEscape(g_status.error),
    JsonEscape(g_status.request_id)
  };
  size_t capacity = 4096;
  for(const auto &field : escaped) {
    if(field.size() > 1024U * 1024U - capacity) return;
    capacity += field.size();
  }
  std::unique_ptr<char[]> body(new(std::nothrow) char[capacity]);
  if(!body) return;
  const bool full = g_status.full_engine;
  const bool rar = g_status.archive_type == "rar" && !full;
  const bool done = g_status.state == "done";
  const bool total_known = full ? done : !rar || done;
  const unsigned long long displayed_total = rar ? (done ? output_bytes : 0) : g_status.total_bytes;
  const unsigned long long displayed_written = rar ? output_bytes : g_status.bytes_written;
  const int displayed_percent = done ? 100 : rar ? 0 : std::min(99, g_status.percent);
  const char *worker_name = "bfpilot-integrated-archive";
  const char *requires_injection = "false";
  int n = snprintf(
      body.get(), capacity,
      "{\"ok\":true,\"worker\":\"%s\","
      "\"state\":\"%s\",\"archiveType\":\"%s\","
      "\"source\":\"%s\",\"destination\":\"%s\",\"stage\":\"%s\","
      "\"current\":\"%s\",\"error\":\"%s\","
      "\"percent\":%d,\"totalBytes\":%llu,\"bytesWritten\":%llu,"
      "\"totalKnown\":%s,\"progressUnit\":\"%s\","
      "\"fullEngine\":%s,\"helperQuiescent\":%s,\"helperTaskId\":%llu,"
      "\"decoderBytesDone\":%llu,\"decoderBytesTotal\":%llu,"
      "\"metricsScope\":\"%s\",\"cpuAvailable\":%s,"
      "\"packedBytesProcessed\":%llu,\"packedTotalBytes\":%llu,"
      "\"filesDone\":%llu,\"totalFiles\":%llu,"
      "\"elapsedMs\":%ld,\"averageMBps\":%.2f,\"errno\":%d,"
      "\"threads\":%u,\"effectiveThreads\":%u,\"threadMode\":\"%s\","
      "\"inputBytes\":%llu,\"outputBytes\":%llu,"
      "\"inputOps\":%llu,\"outputOps\":%llu,"
      "\"inputWaitMs\":%llu,\"outputWaitMs\":%llu,"
      "\"inputMBps\":%.2f,\"outputMBps\":%.2f,"
      "\"inputWaitMBps\":%.2f,\"outputWaitMBps\":%.2f,"
      "\"cpuUserMs\":%llu,\"cpuSystemMs\":%llu,"
      "\"cpuTotalMs\":%llu,\"cpuUtilPercent\":%.2f,"
      "\"voluntarySwitches\":%ld,\"involuntarySwitches\":%ld,"
      "\"maxRss\":%ld,"
      "\"rarMtBatches\":%llu,\"rarMtThreadedBatches\":%llu,"
      "\"rarMtBlocks\":%llu,\"rarMtThreadedBlocks\":%llu,"
      "\"rarMtLargeBlocks\":%llu,"
      "\"priorityNice\":%d,\"priorityRc\":%d,\"priorityErrno\":%d,"
      "\"archiveExitCode\":%d,"
      "\"requiresInjection\":%s,\"requestId\":\"%s\"}",
      worker_name,
      escaped[0].c_str(),
      escaped[1].c_str(),
      escaped[2].c_str(),
      escaped[3].c_str(),
      escaped[4].c_str(),
      escaped[5].c_str(),
      escaped[6].c_str(),
      displayed_percent, displayed_total, displayed_written,
      total_known ? "true" : "false", full && !done ? "decoder-bytes" : "output-bytes",
      full ? "true" : "false", g_status.helper_quiescent ? "true" : "false",
      g_status.helper_task_id, g_status.decoder_done, g_status.decoder_total,
      full ? "coordinator" : "process", full ? "false" : "true",
      rar ? g_status.bytes_written : 0,
      rar ? g_status.total_bytes : 0,
      g_status.files_done, g_status.total_files, elapsed_ms, mbps,
      g_status.errno_code, g_status.threads,
      g_status.effective_threads,
      full ? "decoder-default" : g_status.threads == 0 ? "auto" : "manual",
      input_bytes, output_bytes, input_ops, output_ops,
      input_usec / 1000ULL, output_usec / 1000ULL,
      input_mbps, output_mbps, input_wait_mbps, output_wait_mbps,
      cpu_user_ms, cpu_system_ms, cpu_total_ms, full ? 0.0 : cpu_util_percent,
      voluntary_switches, involuntary_switches, max_rss,
      rar_mt_batches, rar_mt_threaded_batches, rar_mt_blocks,
      rar_mt_threaded_blocks, rar_mt_large_blocks,
      g_status.priority_nice, g_status.priority_rc,
      g_status.priority_errno,
      g_status.archive_exit_code, requires_injection, escaped[7].c_str());
  if(n < 0 || (size_t)n >= capacity) return;
  size_t len = (size_t)n;

  const bool terminal = g_status.state == "done" || g_status.state == "error";
  if(terminal) g_terminal_status_durable = false;
  int fd = open(BFPILOT_ARCHIVE_STATUS_TMP, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0666);
  if(fd < 0) return;
  bool ok = WriteAll(fd, body.get(), len);
  if(ok && terminal && fsync(fd) != 0) ok = false;
  if(close(fd) != 0) ok = false;
  if(ok && rename(BFPILOT_ARCHIVE_STATUS_TMP, BFPILOT_ARCHIVE_STATUS) != 0) ok = false;
  if(terminal) g_terminal_status_durable = ok;
  if(!ok) LogLine("archive status publication failed errno=%d terminal=%d", errno, terminal ? 1 : 0);
}

static void
SetStatus(const std::string &state, const std::string &current, bool force) {
  g_status.state = state;
  g_status.current = current;
  WriteStatus(force);
}

extern "C" void
Ps5UnrarProgress(int percent) {
  if(percent < 0) return;
  if(percent > 100) percent = 100;
  if(percent == g_last_progress) {
    WriteStatus(false);
    return;
  }
  g_last_progress = percent;
  g_status.percent = percent;
  WriteStatus(false);
}

extern "C" void
Ps5ArchiveProgress64(unsigned long long completed, unsigned long long total) {
  if(total > 0) {
    g_status.total_bytes = total;
    g_status.bytes_written = completed > total ? total : completed;
    g_status.percent = (int)((g_status.bytes_written * 100ULL) / total);
    if(g_status.percent > 100) g_status.percent = 100;
  }
  WriteStatus(false);
}

static bool
ReadSmallFile(const std::string &path, std::string &data, size_t max_size) {
  int fd = open(path.c_str(), O_RDONLY);
  if(fd < 0) return false;
  data.clear();
  char buf[4096];
  while(data.size() < max_size) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if(n < 0) {
      if(errno == EINTR) continue;
      close(fd);
      return false;
    }
    if(n == 0) break;
    data.append(buf, (size_t)n);
  }
  close(fd);
  return true;
}

static bool
TimedReadExact(int fd, void *data, size_t size) {
  unsigned long long start = NowUsec();
  bool ok = ReadExact(fd, data, size);
  if(ok) {
    unsigned long long end = NowUsec();
    Ps5ArchiveInputSample((unsigned long long)size, end > start ? end - start : 0);
  }
  return ok;
}

static bool
TimedWriteAll(int fd, const void *data, size_t size) {
  unsigned long long start = NowUsec();
  bool ok = WriteAll(fd, data, size);
  if(ok) {
    unsigned long long end = NowUsec();
    Ps5ArchiveOutputSample((unsigned long long)size, end > start ? end - start : 0);
  }
  return ok;
}


static bool
ArchiveWritebackCheckpoint(int fd, unsigned long long &bytes_since_sync,
                           const char *context, std::string &error) {
  if(bytes_since_sync < BFPILOT_ARCHIVE_WRITEBACK_SYNC_BYTES) return true;
  unsigned long long started = NowUsec();
  if(fsync(fd) != 0) {
    int saved_errno = errno;
    error = "failed to checkpoint extracted file";
    LogLine("archive writeback checkpoint failed context=%s errno=%d",
            context ? context : "output", saved_errno);
    errno = saved_errno;
    return false;
  }
  unsigned long long ended = NowUsec();
  LogLine("archive writeback checkpoint context=%s bytes=%llu elapsed_ms=%llu",
          context ? context : "output", bytes_since_sync,
          ended > started ? (ended - started) / 1000ULL : 0);
  bytes_since_sync = 0;
  return true;
}

static bool
LoadConfig(ArchiveConfig &cfg, std::string &error) {
  cfg.source.clear();
  cfg.destination.clear();
  cfg.password.clear();
  cfg.request_id.clear();
  cfg.threads = 0;
  cfg.priority_nice = BFPILOT_ARCHIVE_NICE;
  cfg.cleanup_partial = false;

  std::string data;
  if(!ReadSmallFile(BFPILOT_ARCHIVE_JOB, data, 64 * 1024)) {
    error = "failed to read " BFPILOT_ARCHIVE_JOB;
    return false;
  }

  size_t pos = 0;
  while(pos < data.size()) {
    size_t end = data.find('\n', pos);
    std::string line =
        data.substr(pos, end == std::string::npos ? std::string::npos
                                                  : end - pos);
    pos = end == std::string::npos ? data.size() : end + 1;
    if(!line.empty() && line.back() == '\r') line.pop_back();
    if(line.empty() || line[0] == '#' || line[0] == ';') continue;
    size_t eq = line.find('=');
    if(eq == std::string::npos) continue;
    std::string key = Trim(line.substr(0, eq));
    // Paths/passwords can legitimately start/end with spaces. Only option
    // syntax is trimmed; private job writers never include CR line endings.
    std::string value = line.substr(eq + 1);
    if(key == "source") cfg.source = value;
    else if(key == "destination") cfg.destination = value;
    else if(key == "password") cfg.password = value;
    else if(key == "requestId") cfg.request_id = value;
    else if(key == "threads") cfg.threads = ParseArchiveThreads(Trim(value));
    else if(key == "nice") cfg.priority_nice = ParseArchiveNice(Trim(value));
    else if(key == "cleanupPartial") {
      cfg.cleanup_partial =
          Trim(value) == "1" || Trim(value) == "true" || Trim(value) == "yes" || Trim(value) == "on";
    }
  }

  if(!IsAllowedArchivePath(cfg.source) ||
     !IsAllowedArchivePath(cfg.destination)) {
    error = "archive job path is outside allowed roots";
    return false;
  }
  if(cfg.source == cfg.destination) {
    error = "source and destination are the same";
    return false;
  }
  if(cfg.source.empty() || cfg.destination.empty()) {
    error = "archive job is missing source or destination";
    return false;
  }
  unsigned requested_threads = cfg.threads;
  cfg.threads = NormalizeArchiveThreads(cfg.threads);
  if(requested_threads != cfg.threads) {
    LogLine("requested threads=%u clamped to %u", requested_threads,
            cfg.threads);
  }
  return true;
}

enum ArchiveType {
  ARCHIVE_UNKNOWN,
  ARCHIVE_RAR,
  ARCHIVE_7Z,
  ARCHIVE_ZIP,
  ARCHIVE_FULL
};

static unsigned
EffectiveArchiveThreadsForType(unsigned requested, ArchiveType type) {
  unsigned threads = EffectiveArchiveThreads(requested);
  if(type == ARCHIVE_RAR && threads > BFPILOT_ARCHIVE_RAR_MAX_THREADS) {
    return BFPILOT_ARCHIVE_RAR_MAX_THREADS;
  }
  return threads;
}

static const char *
ArchiveTypeName(ArchiveType type) {
  switch(type) {
  case ARCHIVE_RAR: return "rar";
  case ARCHIVE_7Z: return "7z";
  case ARCHIVE_ZIP: return "zip";
  case ARCHIVE_FULL: return "auto";
  default: return "unknown";
  }
}

static ArchiveType
DetectArchiveType(const std::string &path) {
  unsigned char magic[8] = {0};
  size_t magic_size = 0;
  int fd = open(path.c_str(), O_RDONLY);
  if(fd >= 0) {
    ssize_t n = read(fd, magic, sizeof(magic));
    if(n > 0) magic_size = (size_t)n;
    close(fd);
  }
  if(magic_size >= 7 && magic[0] == 0x52 && magic[1] == 0x61 &&
     magic[2] == 0x72 && magic[3] == 0x21 && magic[4] == 0x1a &&
     magic[5] == 0x07 && (magic[6] == 0x00 || magic[6] == 0x01)) {
    return ARCHIVE_RAR;
  }
  if(magic_size >= 6 && magic[0] == 0x37 && magic[1] == 0x7a &&
     magic[2] == 0xbc && magic[3] == 0xaf && magic[4] == 0x27 &&
     magic[5] == 0x1c) {
    return ARCHIVE_7Z;
  }
  if(magic_size >= 4 && magic[0] == 'P' && magic[1] == 'K' &&
     (magic[2] == 3 || magic[2] == 5 || magic[2] == 7) &&
     (magic[3] == 4 || magic[3] == 6 || magic[3] == 8)) {
    return ARCHIVE_ZIP;
  }

  std::string name = ToLowerAscii(BaseName(path));
  if(EndsWithNoCase(name, ".rar") ||
     (name.size() > 4 && name[name.size() - 4] == '.' &&
      name[name.size() - 3] == 'r' &&
      name[name.size() - 2] >= '0' && name[name.size() - 2] <= '9' &&
      name[name.size() - 1] >= '0' && name[name.size() - 1] <= '9')) {
    return ARCHIVE_RAR;
  }
  if(EndsWithNoCase(name, ".7z") || EndsWithNoCase(name, ".7z.001")) {
    return ARCHIVE_7Z;
  }
  if(EndsWithNoCase(name, ".zip")) return ARCHIVE_ZIP;
  return ARCHIVE_FULL;
}

static const char *
RarExitReason(RAR_EXIT code) {
  switch(code) {
  case RARX_SUCCESS: return "success";
  case RARX_WARNING: return "warning";
  case RARX_FATAL: return "fatal extraction error";
  case RARX_CRC: return "checksum or password error";
  case RARX_LOCK: return "archive lock error";
  case RARX_WRITE: return "write error";
  case RARX_OPEN: return "open error";
  case RARX_USERERROR: return "bad command or config";
  case RARX_MEMORY: return "out of memory";
  case RARX_CREATE: return "file create error";
  case RARX_NOFILES: return "no files extracted";
  case RARX_BADPWD: return "bad password";
  case RARX_READ: return "read error";
  case RARX_BADARC: return "bad archive";
  case RARX_USERBREAK: return "user break";
  }
  return "unknown error";
}

static std::string
RarExitDiagnostic(RAR_EXIT code, bool password_provided) {
  switch(code) {
  case RARX_BADPWD:
  case RARX_CRC:
    return password_provided
               ? "bad RAR password or checksum error"
               : "RAR password required or archive checksum error";
  case RARX_USERBREAK:
    return password_provided
               ? "RAR extraction aborted; check password and multipart volumes"
               : "RAR password required or next multipart volume is missing";
  case RARX_OPEN:
  case RARX_READ:
    return "RAR open/read error; check that every multipart volume is present next to the .rar";
  default:
    return RarExitReason(code);
  }
}

static int
RunUnrarExtract(const std::string &archive_path, const std::string &dest_path,
                const std::string &password, unsigned threads) {
  ErrHandler.Clean();
  ErrHandler.SetSignalHandlers(false);

  std::unique_ptr<CommandData> cmd(new CommandData);
  cmd->Command = L"X";
  CharToWide(archive_path, cmd->ArcName);
  CharToWide(dest_path, cmd->ExtrPath);
  AddEndSlash(cmd->ExtrPath);
  cmd->AddArcName(cmd->ArcName);
  cmd->FileArgs.AddString(MASKALL);
  cmd->AllYes = true;
  cmd->Overwrite = OVERWRITE_ALL;
  cmd->IgnoreGeneralAttr = true;
  cmd->DisableCopyright = true;
  cmd->DisableDone = true;
  cmd->DisableNames = true;
  cmd->Threads = std::min<uint>(threads > 0 ? threads : 1, MaxPoolThreads);

  if(!password.empty()) {
    std::wstring password_w;
    CharToWide(password, password_w);
    cmd->Password.Set(password_w.c_str());
  }

  uiInit(SOUND_NOTIFY_OFF);

  try {
    CmdExtract extract(cmd.get());
    extract.DoExtract();
  } catch(RAR_EXIT err_code) {
    ErrHandler.SetErrorCode(err_code);
  } catch(std::bad_alloc &) {
    ErrHandler.SetErrorCode(RARX_MEMORY);
  } catch(std::length_error &) {
    ErrHandler.SetErrorCode(RARX_MEMORY);
  } catch(...) {
    ErrHandler.SetErrorCode(RARX_FATAL);
  }

  ErrHandler.MainExit = true;
  return ErrHandler.GetErrorCode();
}

static unsigned short
ReadLe16(const unsigned char *p) {
  return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned int
ReadLe32(const unsigned char *p) {
  return ((unsigned int)p[0]) | ((unsigned int)p[1] << 8) |
         ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned long long
ReadLe64(const unsigned char *p) {
  unsigned long long lo = ReadLe32(p);
  unsigned long long hi = ReadLe32(p + 4);
  return lo | (hi << 32);
}

static bool
U64AddOverflow(unsigned long long a, unsigned long long b,
               unsigned long long &out) {
  if(ULLONG_MAX - a < b) return true;
  out = a + b;
  return false;
}

class ZipCrypto {
  unsigned int keys[3];

  static unsigned int crc_update(unsigned int crc, unsigned char ch) {
    static unsigned int table[256];
    static bool ready = false;
    if(!ready) {
      for(unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for(unsigned int j = 0; j < 8; j++) {
          c = (c & 1U) ? (0xedb88320U ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
      }
      ready = true;
    }
    return (crc >> 8) ^ table[(crc ^ ch) & 0xffU];
  }

  void update(unsigned char ch) {
    keys[0] = crc_update(keys[0], ch);
    keys[1] = keys[1] + (keys[0] & 0xff);
    keys[1] = keys[1] * 134775813U + 1U;
    unsigned char high = (unsigned char)(keys[1] >> 24);
    keys[2] = crc_update(keys[2], high);
  }

  unsigned char decrypt_byte(void) const {
    unsigned int t = keys[2] | 2U;
    return (unsigned char)(((t * (t ^ 1U)) >> 8) & 0xff);
  }

public:
  ZipCrypto(const std::string &password) {
    keys[0] = 305419896U;
    keys[1] = 591751049U;
    keys[2] = 878082192U;
    for(size_t i = 0; i < password.size(); i++) {
      update((unsigned char)password[i]);
    }
  }

  unsigned char decrypt(unsigned char ch) {
    unsigned char plain = (unsigned char)(ch ^ decrypt_byte());
    update(plain);
    return plain;
  }

  void decrypt_buffer(unsigned char *buf, size_t size) {
    for(size_t i = 0; i < size; i++) buf[i] = decrypt(buf[i]);
  }
};

struct ZipEntry {
  std::string name;
  unsigned short flags = 0;
  unsigned short method = 0;
  unsigned short mod_time = 0;
  unsigned int crc32 = 0;
  unsigned long long compressed_size = 0;
  unsigned long long uncompressed_size = 0;
  unsigned long long local_offset = 0;
  unsigned int disk_number = 0;
  bool is_dir = false;
  bool encrypted = false;
  bool aes_encrypted = false;
};

struct ZipVolumeInfo {
  std::string path;
  unsigned long long size = 0;
  unsigned long long start_offset = 0;
};

class ZipMultiVolume {
public:
  std::vector<ZipVolumeInfo> volumes;
  unsigned long long total_size;
  bool is_split;
  unsigned long long stream_pos;

  ZipMultiVolume() : total_size(0), is_split(false), stream_pos(0) {}
  ~ZipMultiVolume() {
    Close();
  }

  void Close() {
    for(size_t i = 0; i < fds.size(); i++) {
      if(fds[i] >= 0) close(fds[i]);
    }
    fds.clear();
    volumes.clear();
    total_size = 0;
    is_split = false;
    stream_pos = 0;
  }

  bool Open(const std::string &path, std::string &error) {
    Close();

    std::string lower_path = path;
    for(size_t i = 0; i < lower_path.size(); i++) {
      lower_path[i] = (char)tolower((unsigned char)lower_path[i]);
    }

    std::string dir = DirName(path);
    std::string base = BaseName(path);

    std::vector<std::string> candidate_paths;

    if(lower_path.size() > 4 && lower_path.substr(lower_path.size() - 4) == ".zip") {
      std::string prefix = JoinPath(dir, base.substr(0, base.size() - 4));
      struct stat st;
      if(stat((prefix + ".z01").c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        is_split = true;
        for(int part = 1; part <= 9999; part++) {
          char ext[32];
          snprintf(ext, sizeof(ext), ".z%02d", part);
          std::string p = prefix + ext;
          if(stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            snprintf(ext, sizeof(ext), ".Z%02d", part);
            p = prefix + ext;
            if(stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
              break;
            }
          }
          candidate_paths.push_back(p);
        }
        candidate_paths.push_back(path);
      }
    } else if(lower_path.size() > 4 && lower_path.substr(lower_path.size() - 4) == ".001") {
      std::string prefix = JoinPath(dir, base.substr(0, base.size() - 4));
      struct stat st;
      if(stat((prefix + ".001").c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        is_split = true;
        for(int part = 1; part <= 9999; part++) {
          char ext[32];
          snprintf(ext, sizeof(ext), ".%03d", part);
          std::string p = prefix + ext;
          if(stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            candidate_paths.push_back(p);
          } else {
            break;
          }
        }
      }
    }

    if(candidate_paths.empty()) {
      struct stat st;
      if(stat(path.c_str(), &st) != 0) {
        error = "failed to stat zip archive: " + path;
        return false;
      }
      ZipVolumeInfo vol;
      vol.path = path;
      vol.size = (unsigned long long)st.st_size;
      vol.start_offset = 0;
      volumes.push_back(vol);
      total_size = vol.size;
    } else {
      for(size_t i = 0; i < candidate_paths.size(); i++) {
        struct stat st;
        if(stat(candidate_paths[i].c_str(), &st) != 0) {
          error = "failed to stat volume: " + candidate_paths[i];
          return false;
        }
        ZipVolumeInfo vol;
        vol.path = candidate_paths[i];
        vol.size = (unsigned long long)st.st_size;
        vol.start_offset = total_size;
        total_size += vol.size;
        volumes.push_back(vol);
      }
    }

    for(size_t i = 0; i < volumes.size(); i++) {
      int fd = open(volumes[i].path.c_str(), O_RDONLY);
      if(fd < 0) {
        error = "failed to open volume: " + volumes[i].path;
        Close();
        return false;
      }
#ifdef POSIX_FADV_SEQUENTIAL
      (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
      fds.push_back(fd);
    }

    stream_pos = 0;
    return true;
  }

  bool Seek(unsigned long long pos) {
    if(pos > total_size) return false;
    stream_pos = pos;
    return true;
  }

  bool Read(void *data, size_t size) {
    if(!ReadExactAt(stream_pos, data, size)) return false;
    stream_pos += size;
    return true;
  }

  bool ReadExactAt(unsigned long long offset, void *data, size_t size) {
    if(size == 0) return true;
    if(offset + size > total_size) return false;

    unsigned char *ptr = (unsigned char *)data;
    size_t remaining = size;
    unsigned long long cur = offset;

    while(remaining > 0) {
      int vol_idx = -1;
      for(size_t i = 0; i < volumes.size(); i++) {
        if(cur >= volumes[i].start_offset && cur < volumes[i].start_offset + volumes[i].size) {
          vol_idx = (int)i;
          break;
        }
      }
      if(vol_idx < 0) return false;

      unsigned long long local = cur - volumes[vol_idx].start_offset;
      unsigned long long vol_rem = volumes[vol_idx].size - local;
      size_t chunk = remaining < vol_rem ? remaining : (size_t)vol_rem;

      int fd = fds[(size_t)vol_idx];
      if(lseek(fd, (off_t)local, SEEK_SET) < 0) return false;

      size_t done = 0;
      while(done < chunk) {
        ssize_t n = read(fd, ptr + done, chunk - done);
        if(n < 0) {
          if(errno == EINTR) continue;
          return false;
        }
        if(n == 0) return false;
        done += (size_t)n;
      }

      ptr += chunk;
      remaining -= chunk;
      cur += chunk;
    }
    return true;
  }

private:
  std::vector<int> fds;
};

struct ZipIoBuffers {
  unsigned char *in;
  unsigned char *out;

  ZipIoBuffers() : in(NULL), out(NULL) {}
  ZipIoBuffers(const ZipIoBuffers &) = delete;
  ZipIoBuffers &operator=(const ZipIoBuffers &) = delete;
  ~ZipIoBuffers() {
    free(in);
    free(out);
  }

  bool Init(std::string &error) {
    if(posix_memalign((void **)&in, BFPILOT_ARCHIVE_IO_ALIGNMENT,
                      BFPILOT_ZIP_IN_BUFFER_SIZE) != 0 ||
       posix_memalign((void **)&out, BFPILOT_ARCHIVE_IO_ALIGNMENT,
                      BFPILOT_ZIP_OUT_BUFFER_SIZE) != 0) {
      free(in);
      free(out);
      in = NULL;
      out = NULL;
      error = "out of memory";
      return false;
    }
    LogLine("zip io buffers input=%u output=%u alignment=%u",
            BFPILOT_ZIP_IN_BUFFER_SIZE, BFPILOT_ZIP_OUT_BUFFER_SIZE,
            BFPILOT_ARCHIVE_IO_ALIGNMENT);
    return true;
  }
};

static bool
ReadExactAt(ZipMultiVolume &mv, unsigned long long offset, void *data, size_t size) {
  return mv.ReadExactAt(offset, data, size);
}

static bool
TimedReadExact(ZipMultiVolume &mv, void *data, size_t size) {
  unsigned long long start = NowUsec();
  bool ok = mv.Read(data, size);
  if(ok) {
    unsigned long long end = NowUsec();
    Ps5ArchiveInputSample((unsigned long long)size, end > start ? end - start : 0);
  }
  return ok;
}

static bool
ParseZip64Extra(const std::vector<unsigned char> &extra, ZipEntry &entry,
                bool need_uncomp, bool need_comp, bool need_offset) {
  size_t pos = 0;
  while(pos + 4 <= extra.size()) {
    unsigned short header_id = ReadLe16(&extra[pos]);
    unsigned short data_size = ReadLe16(&extra[pos + 2]);
    pos += 4;
    if(pos + data_size > extra.size()) return false;
    if(header_id == 0x0001) {
      size_t p = pos;
      if(need_uncomp) {
        if(p + 8 > pos + data_size) return false;
        entry.uncompressed_size = ReadLe64(&extra[p]);
        p += 8;
      }
      if(need_comp) {
        if(p + 8 > pos + data_size) return false;
        entry.compressed_size = ReadLe64(&extra[p]);
        p += 8;
      }
      if(need_offset) {
        if(p + 8 > pos + data_size) return false;
        entry.local_offset = ReadLe64(&extra[p]);
      }
      return true;
    }
    pos += data_size;
  }
  return !(need_uncomp || need_comp || need_offset);
}

static bool
ZipExtraHasAes(const std::vector<unsigned char> &extra) {
  size_t pos = 0;
  while(pos + 4 <= extra.size()) {
    unsigned short header_id = ReadLe16(&extra[pos]);
    unsigned short data_size = ReadLe16(&extra[pos + 2]);
    pos += 4;
    if(pos + data_size > extra.size()) return false;
    if(header_id == 0x9901) return true;
    pos += data_size;
  }
  return false;
}

static bool
FindZipCentralDir(ZipMultiVolume &mv, unsigned long long file_size,
                  unsigned long long &central_offset,
                  unsigned long long &central_size,
                  unsigned long long &entry_count,
                  std::string &error) {
  size_t tail_size =
      (size_t)std::min<unsigned long long>(file_size, 22ULL + 65535ULL + 128ULL);
  std::vector<unsigned char> tail(tail_size);
  if(!ReadExactAt(mv, file_size - tail_size, tail.data(), tail_size)) {
    error = "failed to read zip end of central directory";
    return false;
  }

  ssize_t eocd = -1;
  for(ssize_t i = (ssize_t)tail_size - 22; i >= 0; i--) {
    if(ReadLe32(&tail[(size_t)i]) == ZIP_SIG_END) {
      eocd = i;
      break;
    }
  }
  if(eocd < 0) {
    error = "zip end of central directory not found";
    return false;
  }

  const unsigned char *p = &tail[(size_t)eocd];
  unsigned int entries16 = ReadLe16(p + 10);
  unsigned int size32 = ReadLe32(p + 12);
  unsigned int offset32 = ReadLe32(p + 16);
  central_size = size32;
  entry_count = entries16;

  unsigned int disk_with_cd = ReadLe16(p + 6);
  if(mv.is_split && disk_with_cd < mv.volumes.size()) {
    central_offset = mv.volumes[disk_with_cd].start_offset + offset32;
  } else {
    central_offset = offset32;
  }

  bool needs_zip64 = entries16 == 0xffffU || size32 == 0xffffffffU ||
                     offset32 == 0xffffffffU;
  if(!needs_zip64) return true;

  unsigned long long eocd_abs = file_size - tail_size + (unsigned long long)eocd;
  if(eocd_abs < 20) {
    error = "zip64 locator missing";
    return false;
  }
  unsigned char locator[20];
  if(!ReadExactAt(mv, eocd_abs - 20, locator, sizeof(locator)) ||
     ReadLe32(locator) != ZIP_SIG_ZIP64_LOCATOR) {
    error = "zip64 locator missing";
    return false;
  }
  unsigned long long zip64_eocd_offset_raw = ReadLe64(locator + 8);
  unsigned int disk_with_zip64_eocd = ReadLe32(locator + 4);
  unsigned long long zip64_eocd_offset = zip64_eocd_offset_raw;
  if(mv.is_split && disk_with_zip64_eocd < mv.volumes.size()) {
    zip64_eocd_offset = mv.volumes[disk_with_zip64_eocd].start_offset + zip64_eocd_offset_raw;
  }

  unsigned char zip64[56];
  if(!ReadExactAt(mv, zip64_eocd_offset, zip64, sizeof(zip64)) ||
     ReadLe32(zip64) != ZIP_SIG_ZIP64_END) {
    error = "zip64 end of central directory missing";
    return false;
  }
  entry_count = ReadLe64(zip64 + 32);
  central_size = ReadLe64(zip64 + 40);
  unsigned long long zip64_offset_raw = ReadLe64(zip64 + 48);
  unsigned int zip64_disk_with_cd = ReadLe32(zip64 + 20);
  if(mv.is_split && zip64_disk_with_cd < mv.volumes.size()) {
    central_offset = mv.volumes[zip64_disk_with_cd].start_offset + zip64_offset_raw;
  } else {
    central_offset = zip64_offset_raw;
  }
  return true;
}

static bool
ReadZipEntries(ZipMultiVolume &mv, unsigned long long central_offset,
               unsigned long long central_size,
               unsigned long long entry_count, std::vector<ZipEntry> &entries,
               std::string &error) {
  unsigned long long offset = central_offset;
  unsigned long long central_end = 0;
  if(U64AddOverflow(central_offset, central_size, central_end)) {
    error = "bad zip central directory size";
    return false;
  }
  for(unsigned long long i = 0; i < entry_count; i++) {
    unsigned long long min_entry_end = 0;
    if(U64AddOverflow(offset, 46ULL, min_entry_end) ||
       min_entry_end > central_end) {
      error = "zip central directory is truncated";
      return false;
    }
    unsigned char hdr[46];
    if(!ReadExactAt(mv, offset, hdr, sizeof(hdr)) ||
       ReadLe32(hdr) != ZIP_SIG_CENTRAL) {
      error = "bad zip central directory entry";
      return false;
    }
    ZipEntry entry;
    entry.flags = ReadLe16(hdr + 8);
    entry.method = ReadLe16(hdr + 10);
    entry.mod_time = ReadLe16(hdr + 12);
    entry.crc32 = ReadLe32(hdr + 16);
    entry.compressed_size = ReadLe32(hdr + 20);
    entry.uncompressed_size = ReadLe32(hdr + 24);
    unsigned short name_len = ReadLe16(hdr + 28);
    unsigned short extra_len = ReadLe16(hdr + 30);
    unsigned short comment_len = ReadLe16(hdr + 32);
    entry.disk_number = ReadLe16(hdr + 34);
    entry.local_offset = ReadLe32(hdr + 42);
    entry.encrypted = (entry.flags & 1) != 0;

    if(name_len == 0 || name_len > BFPILOT_ZIP_MAX_MEMBER_PATH) {
      error = "zip member path length is unsupported";
      return false;
    }
    unsigned long long variable_len =
        (unsigned long long)name_len + extra_len + comment_len;
    unsigned long long entry_end = 0;
    if(U64AddOverflow(min_entry_end, variable_len, entry_end) ||
       entry_end > central_end) {
      error = "zip central directory entry is truncated";
      return false;
    }

    std::vector<unsigned char> name_buf(name_len);
    std::vector<unsigned char> extra(extra_len);
    if(name_len > 0 &&
       !ReadExactAt(mv, offset + 46, name_buf.data(), name_len)) {
      error = "failed to read zip file name";
      return false;
    }
    if(extra_len > 0 &&
       !ReadExactAt(mv, offset + 46 + name_len, extra.data(), extra_len)) {
      error = "failed to read zip extra data";
      return false;
    }
    entry.name.assign((const char *)name_buf.data(), name_buf.size());
    entry.name = NormalizeMemberPath(entry.name);
    entry.is_dir = !entry.name.empty() && entry.name[entry.name.size() - 1] == '/';
    entry.aes_encrypted = entry.encrypted &&
                          (entry.method == 99 || ZipExtraHasAes(extra));

    bool need_uncomp = entry.uncompressed_size == 0xffffffffULL;
    bool need_comp = entry.compressed_size == 0xffffffffULL;
    bool need_offset = entry.local_offset == 0xffffffffULL;
    if((need_uncomp || need_comp || need_offset) &&
       !ParseZip64Extra(extra, entry, need_uncomp, need_comp, need_offset)) {
      error = "bad or missing zip64 extra data";
      return false;
    }

    if(!IsSafeMemberPath(entry.name)) {
      error = "unsafe zip member path: " + entry.name;
      return false;
    }

    entries.push_back(entry);
    offset = entry_end;
  }
  return true;
}

static bool
OpenZipEntryData(ZipMultiVolume &mv, const ZipEntry &entry, unsigned long long &data_offset,
                 unsigned long long archive_size, std::string &error) {
  unsigned char hdr[30];
  unsigned long long local_offset = entry.local_offset;
  bool sig_ok = false;

  if(mv.is_split && entry.disk_number < mv.volumes.size()) {
    unsigned long long test_offset = mv.volumes[entry.disk_number].start_offset + entry.local_offset;
    if(ReadExactAt(mv, test_offset, hdr, sizeof(hdr)) && ReadLe32(hdr) == ZIP_SIG_LOCAL) {
      local_offset = test_offset;
      sig_ok = true;
    }
  }

  if(!sig_ok) {
    if(ReadExactAt(mv, entry.local_offset, hdr, sizeof(hdr)) && ReadLe32(hdr) == ZIP_SIG_LOCAL) {
      local_offset = entry.local_offset;
      sig_ok = true;
    }
  }

  if(!sig_ok) {
    error = "bad zip local file header signature";
    return false;
  }

  unsigned short name_len = ReadLe16(hdr + 26);
  unsigned short extra_len = ReadLe16(hdr + 28);
  unsigned long long header_len = 30ULL + name_len + extra_len;
  if(U64AddOverflow(local_offset, header_len, data_offset) ||
     data_offset > archive_size) {
    error = "zip local file header is outside archive";
    return false;
  }
  return true;
}

static bool
WriteZipStoredFile(ZipMultiVolume &mv, int out_fd, ZipEntry &entry,
                    unsigned long long data_offset, ZipCrypto *crypto,
                    ZipIoBuffers &buffers, std::string &error) {
  unsigned char *buf = buffers.in;
  unsigned long long remaining = entry.compressed_size;
  mz_ulong crc = MZ_CRC32_INIT;
  unsigned long long written = 0;
  unsigned long long bytes_since_sync = 0;

  if(!mv.Seek(data_offset)) {
    error = "failed to seek stored zip data";
    return false;
  }
  while(remaining > 0) {
    extern int g_archive_cancel;
    if(__atomic_load_n(&g_archive_cancel, __ATOMIC_RELAXED)) {
      error = "archive: cancelled";
      return false;
    }
    size_t want =
        (size_t)std::min<unsigned long long>(remaining,
                                             BFPILOT_ZIP_IN_BUFFER_SIZE);
    if(!TimedReadExact(mv, buf, want)) {
      error = "failed to read stored zip data";
      return false;
    }
    if(crypto) crypto->decrypt_buffer(buf, want);
    if(!TimedWriteAll(out_fd, buf, want)) {
      error = "failed to write extracted zip file";
      return false;
    }
    bytes_since_sync += want;
    if(!ArchiveWritebackCheckpoint(out_fd, bytes_since_sync, "zip-stored",
                                   error)) {
      return false;
    }
    crc = mz_crc32(crc, buf, want);
    remaining -= want;
    written += want;
    g_status.bytes_written += want;
    g_status.percent = g_status.total_bytes > 0
                           ? (int)((g_status.bytes_written * 100ULL) /
                                   g_status.total_bytes)
                           : g_status.percent;
    WriteStatus(false);
  }

  if(written != entry.uncompressed_size) {
    error = "stored zip size mismatch";
    return false;
  }
  if((unsigned int)crc != entry.crc32) {
    error = "zip checksum or password error";
    return false;
  }
  return true;
}

static bool
WriteZipDeflatedFile(ZipMultiVolume &mv, int out_fd, ZipEntry &entry,
                      unsigned long long data_offset, ZipCrypto *crypto,
                      ZipIoBuffers &buffers, std::string &error) {
  unsigned char *inbuf = buffers.in;
  unsigned char *outbuf = buffers.out;
  unsigned long long remaining = entry.compressed_size;
  unsigned long long written = 0;
  unsigned long long bytes_since_sync = 0;
  mz_ulong crc = MZ_CRC32_INIT;

  mz_stream stream;
  memset(&stream, 0, sizeof(stream));
  int zrc = mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS);
  if(zrc != MZ_OK) {
    error = "zip deflate init failed";
    return false;
  }

  if(!mv.Seek(data_offset)) {
    mz_inflateEnd(&stream);
    error = "failed to seek deflated zip data";
    return false;
  }

  bool ok = true;
  bool done = false;
  while(!done) {
    extern int g_archive_cancel;
    if(__atomic_load_n(&g_archive_cancel, __ATOMIC_RELAXED)) {
      error = "archive: cancelled";
      ok = false;
      break;
    }
    if(stream.avail_in == 0 && remaining > 0) {
      size_t want =
          (size_t)std::min<unsigned long long>(remaining,
                                               BFPILOT_ZIP_IN_BUFFER_SIZE);
      if(!TimedReadExact(mv, inbuf, want)) {
        error = "failed to read deflated zip data";
        ok = false;
        break;
      }
      if(crypto) crypto->decrypt_buffer(inbuf, want);
      remaining -= want;
      stream.next_in = inbuf;
      stream.avail_in = (mz_uint)want;
    }

    unsigned long long remaining_out =
        entry.uncompressed_size > written ? entry.uncompressed_size - written
                                          : 0;
    int flush =
        (remaining == 0 && remaining_out <= BFPILOT_ZIP_OUT_BUFFER_SIZE)
                     ? MZ_FINISH
                     : MZ_NO_FLUSH;
    stream.next_out = outbuf;
    stream.avail_out = (mz_uint)BFPILOT_ZIP_OUT_BUFFER_SIZE;
    zrc = mz_inflate(&stream, flush);
    size_t produced = BFPILOT_ZIP_OUT_BUFFER_SIZE - stream.avail_out;
    if(produced > 0) {
      if(written > entry.uncompressed_size ||
         produced > entry.uncompressed_size - written) {
        error = "deflated zip size exceeds header";
        ok = false;
        break;
      }
      if(!TimedWriteAll(out_fd, outbuf, produced)) {
        error = "failed to write extracted zip file";
        ok = false;
        break;
      }
      bytes_since_sync += produced;
      if(!ArchiveWritebackCheckpoint(out_fd, bytes_since_sync,
                                     "zip-deflate", error)) {
        ok = false;
        break;
      }
      crc = mz_crc32(crc, outbuf, produced);
      written += produced;
      g_status.bytes_written += produced;
      g_status.percent = g_status.total_bytes > 0
                             ? (int)((g_status.bytes_written * 100ULL) /
                                     g_status.total_bytes)
                             : g_status.percent;
      WriteStatus(false);
    }

    if(zrc == MZ_STREAM_END) {
      done = true;
    } else if(zrc != MZ_OK && zrc != MZ_BUF_ERROR) {
      error = "zip deflate or password error";
      ok = false;
      break;
    } else if(remaining == 0 && stream.avail_in == 0 && produced == 0) {
      error = "zip deflate ended unexpectedly";
      ok = false;
      break;
    }
  }

  mz_inflateEnd(&stream);
  if(!ok) return false;
  if(written != entry.uncompressed_size) {
    error = "deflated zip size mismatch";
    return false;
  }
  if((unsigned int)crc != entry.crc32) {
    error = "zip checksum or password error";
    return false;
  }
  return true;
}

static bool
ExtractOneZipEntry(ZipMultiVolume &mv, ZipEntry &entry, const std::string &dest,
                   const std::string &password, ZipIoBuffers &buffers,
                   unsigned long long archive_size, std::string &error) {
  std::string out_path = JoinPath(dest, entry.name);
  g_status.current = entry.name;

  if(entry.is_dir) {
    if(!MkdirAll(out_path)) {
      error = "failed to create zip directory: " + entry.name;
      return false;
    }
    g_status.files_done++;
    WriteStatus(false);
    return true;
  }

  if(entry.aes_encrypted) {
    error = "zip AES encryption is not supported yet";
    return false;
  }
  if(entry.encrypted && password.empty()) {
    error = "zip entry needs a password";
    return false;
  }
  if(entry.method != 0 && entry.method != MZ_DEFLATED) {
    char buf[96];
    snprintf(buf, sizeof(buf), "unsupported zip compression method %u",
             (unsigned)entry.method);
    error = buf;
    return false;
  }

  unsigned long long data_offset = 0;
  if(!OpenZipEntryData(mv, entry, data_offset, archive_size, error)) {
    return false;
  }
  if(entry.compressed_size > archive_size ||
     data_offset > archive_size - entry.compressed_size) {
    error = "zip entry data is outside archive";
    return false;
  }

  ZipCrypto zip_crypto(password);
  ZipCrypto *crypto = entry.encrypted ? &zip_crypto : NULL;
  if(entry.encrypted) {
    if(entry.compressed_size < 12) {
      error = "bad encrypted zip entry";
      return false;
    }
    unsigned char header[12];
    if(!ReadExactAt(mv, data_offset, header, sizeof(header))) {
      error = "failed to read encrypted zip header";
      return false;
    }
    crypto->decrypt_buffer(header, sizeof(header));
    unsigned char crc_check = (unsigned char)(entry.crc32 >> 24);
    unsigned char time_check = (unsigned char)(entry.mod_time >> 8);
    if(header[11] != crc_check && header[11] != time_check) {
      error = "bad zip password";
      return false;
    }
    data_offset += 12;
    entry.compressed_size -= 12;
    if(entry.compressed_size > archive_size ||
       data_offset > archive_size - entry.compressed_size) {
      error = "encrypted zip entry data is outside archive";
      return false;
    }
  }

  if(!MkdirAll(DirName(out_path))) {
    error = "failed to create zip output directory";
    return false;
  }

  int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if(out_fd < 0) {
    error = "failed to create zip output file";
    return false;
  }
  if(!PreallocateOutputFile(out_fd, entry.uncompressed_size, "zip", error)) {
    int saved_errno = errno;
    close(out_fd);
    errno = saved_errno;
    return false;
  }

  bool ok = false;
  if(entry.method == 0) {
    ok = WriteZipStoredFile(mv, out_fd, entry, data_offset, crypto,
                            buffers, error);
  } else {
    ok = WriteZipDeflatedFile(mv, out_fd, entry, data_offset, crypto,
                              buffers, error);
  }

  if(close(out_fd) != 0 && ok) {
    error = "failed to close zip output file";
    ok = false;
  }
  if(ok) g_status.files_done++;
  WriteStatus(false);
  return ok;
}

struct ActiveFullArchive {
  unsigned long long id = 0, device = 0, inode = 0;
  std::string source, destination, stage;
  bool stage_missing = false, published = false;
};
static int SaveActiveFullArchive(uint64_t id) {
  struct stat st;
  if(lstat(g_status.stage.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return -(errno ? errno : ENOTDIR);
  std::string data = "BFPILOT_FULL_ARCHIVE_V1\n";
  char numbers[192];
  snprintf(numbers,sizeof(numbers),"id=%llu\ndevice=%llu\ninode=%llu\n",
      (unsigned long long)id,(unsigned long long)st.st_dev,(unsigned long long)st.st_ino);
  data += numbers;
  data += "source=" + g_status.source + "\ndestination=" + g_status.destination +
          "\nstage=" + g_status.stage + "\n";
  int fd = open(BFPILOT_FULL_ACTIVE_TMP, O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
  if(fd<0)return -errno;
  int error=0;
  if(!WriteAll(fd,data.data(),data.size()) || fsync(fd)!=0)error=errno ? errno : EIO;
  if(close(fd)!=0 && !error)error=errno;
  if(!error && rename(BFPILOT_FULL_ACTIVE_TMP,BFPILOT_FULL_ACTIVE)!=0)error=errno;
  if(error)unlink(BFPILOT_FULL_ACTIVE_TMP);
  if(!error)g_full_marker_pending=true;
  return -error;
}
static void ClearActiveFullArchive() {
  if(!g_status.helper_quiescent)return;
  if(unlink(BFPILOT_FULL_ACTIVE)!=0 && errno!=ENOENT) {
    LogLine("cannot remove completed full archive marker errno=%d",errno);
    return;
  }
  g_full_marker_pending=false;
  if(g_status.helper_task_admitted) {
    int rc=bfpilot_full_archive_ack(g_status.helper_task_id);
    if(rc)LogLine("completed full archive acknowledgement failed task=%llu rc=%d",
                  g_status.helper_task_id,rc);
    else g_status.helper_task_admitted=false;
  }
}
static bool LoadActiveFullArchive(ActiveFullArchive &job, std::string &error) {
  std::string text;
  if(!ReadSmallFile(BFPILOT_FULL_ACTIVE,text,16384) ||
     text.compare(0,24,"BFPILOT_FULL_ARCHIVE_V1\n")!=0) {
    error="invalid active archive marker";return false;
  }
  size_t start=text.find('\n')+1;
  unsigned seen=0;
  while(start<text.size()) {
    size_t end=text.find('\n',start);
    if(end==std::string::npos){error="truncated active archive marker";return false;}
    std::string line=text.substr(start,end-start);start=end+1;
    size_t equal=line.find('=');
    if(equal==std::string::npos){error="invalid active archive field";return false;}
    std::string key=line.substr(0,equal),value=line.substr(equal+1);
    unsigned bit=0;
    if(key=="id" || key=="device" || key=="inode") {
      char *tail=NULL;errno=0;
      unsigned long long number=strtoull(value.c_str(),&tail,10);
      if(errno || value.empty() || !tail || *tail){error="invalid active archive identity";return false;}
      if(key=="id"){job.id=number;bit=1;}else if(key=="device"){job.device=number;bit=2;}else{job.inode=number;bit=4;}
    } else if(key=="source"){job.source=value;bit=8;}
    else if(key=="destination"){job.destination=value;bit=16;}
    else if(key=="stage"){job.stage=value;bit=32;}
    else {error="unknown active archive field";return false;}
    if(seen&bit){error="duplicate active archive field";return false;}seen|=bit;
  }
  const bool stage_name=job.stage.find("/BFpilot/staging/extract-")!=std::string::npos ||
      job.stage.find("/.bfpilot-staging/extract-")!=std::string::npos;
  if(seen!=63 || !job.id || !IsAllowedArchivePath(job.source) ||
     !IsAllowedArchivePath(job.destination) || !IsAllowedArchivePath(job.stage) || !stage_name) {
    error="active archive paths or identity are invalid";return false;
  }
  struct stat st;
  if(lstat(job.stage.c_str(),&st)!=0) {
    if(errno!=ENOENT){error="cannot verify active archive staging identity";return false;}
    job.stage_missing=true;
    // A coordinator can exit after the atomic publication rename but before
    // clearing its marker. Only the very same directory is our published tree.
    job.published=lstat(job.destination.c_str(),&st)==0 && S_ISDIR(st.st_mode) &&
        (unsigned long long)st.st_dev==job.device && (unsigned long long)st.st_ino==job.inode;
    return true;
  }
  if(!S_ISDIR(st.st_mode) ||
     (unsigned long long)st.st_dev!=job.device || (unsigned long long)st.st_ino!=job.inode) {
    error="active archive staging identity changed; retained for inspection";return false;
  }
  return true;
}

static int RunFullArchive(const std::string &source, const std::string &stage,
                          const std::string &password, std::string &error, uint64_t resume_id = 0) {
  g_status.full_engine = true;
  g_status.effective_threads = 0; // The service uses its decoder's bounded defaults.
  bfpilot_full_archive_callbacks_t callbacks = {};
  callbacks.started = [](void *, uint64_t id) -> int {
    int rc=SaveActiveFullArchive(id);
    if(rc)return rc;
    g_status.helper_task_id = id;
    g_status.helper_quiescent = false;
    WriteStatus(true);
    return 0;
  };
  callbacks.accepted = [](void *) { g_status.helper_task_admitted=true; };
  callbacks.cancelled = [](void *) -> int {
    return __atomic_load_n(&g_archive_cancel, __ATOMIC_RELAXED);
  };
  callbacks.progress = [](void *, uint64_t done, uint64_t total) {
    g_status.decoder_done = done;
    g_status.decoder_total = total;
    // Decoder totals are not universally output byte totals. Label separately.
    g_status.percent = total ? (int)std::min<long double>(99,
        ((long double)done * 100) / (long double)total) : 0;
    WriteStatus(false);
  };
  callbacks.current = [](void *, const char *path) {
    g_status.current = path;
    WriteStatus(false);
  };
  char message[1024] = {0};
  int quiet = 1;
  int rc = resume_id ? bfpilot_full_archive_resume(resume_id,source.c_str(),stage.c_str(),
      &callbacks,message,sizeof(message),&quiet) :
      bfpilot_full_archive_extract(source.c_str(), stage.c_str(), password.c_str(),
      &callbacks, message, sizeof(message), &quiet);
  g_status.helper_quiescent = quiet != 0;
  if(rc) {
    errno = -rc;
    error = rc == -ECANCELED ? "archive: cancelled" :
        message[0] ? message : strerror(-rc);
    return rc == -ECANCELED ? RARX_USERBREAK : RARX_FATAL;
  }
  return 0;
}

static bool
RunZipExtract(const std::string &archive_path, const std::string &dest_path,
              const std::string &password, bool automatic, std::string &error) {
  ZipMultiVolume mv;
  if(!mv.Open(archive_path, error)) {
    return false;
  }

  unsigned long long central_offset = 0;
  unsigned long long central_size = 0;
  unsigned long long entry_count = 0;
  if(!FindZipCentralDir(mv, mv.total_size, central_offset,
                        central_size, entry_count, error)) {
    return false;
  }
  unsigned long long central_end = 0;
  if(U64AddOverflow(central_offset, central_size, central_end) ||
     central_end > mv.total_size) {
    error = "zip central directory is outside archive";
    return false;
  }
  if(entry_count > BFPILOT_ZIP_MAX_ENTRIES) {
    error = "zip has too many entries";
    return false;
  }

  std::vector<ZipEntry> entries;
  entries.reserve((size_t)std::min<unsigned long long>(entry_count, 4096ULL));
  if(!ReadZipEntries(mv, central_offset, central_size, entry_count, entries,
                     error)) {
    return false;
  }

  // Route unsupported ZIP features before any output is written, preserving
  // the native stored/deflate path for ordinary archives.
  for(const auto &entry : entries) {
    if(entry.aes_encrypted || (entry.method != 0 && entry.method != MZ_DEFLATED))
      return RunFullArchive(archive_path, dest_path, password, error) == 0;
  }
  (void)central_size;
  g_status.total_files = entries.size();
  g_status.total_bytes = 0;
  for(size_t i = 0; i < entries.size(); i++) {
    if(!entries[i].is_dir) {
      unsigned long long next_total = 0;
      if(U64AddOverflow(g_status.total_bytes, entries[i].uncompressed_size,
                        next_total)) {
        error = "zip uncompressed size is too large";
        return false;
      }
      g_status.total_bytes = next_total;
    }
  }
  if(!CheckDestinationSpace(dest_path, g_status.total_bytes, "zip", error)) {
    return false;
  }
  // Keep the central-directory limits and capacity preflight above. The full
  // engine overlaps bounded read-ahead, decoding and output writes; explicit
  // manual requests retain the original integrated decoder and telemetry.
  if(automatic)
    return RunFullArchive(archive_path, dest_path, password, error) == 0;
  WriteStatus(true);

  if(!MkdirAll(dest_path)) {
    error = "failed to create zip destination";
    return false;
  }

  ZipIoBuffers buffers;
  if(!buffers.Init(error)) {
    return false;
  }

  for(size_t i = 0; i < entries.size(); i++) {
    extern int g_archive_cancel;
    if(__atomic_load_n(&g_archive_cancel, __ATOMIC_RELAXED)) {
      error = "archive: cancelled";
      return false;
    }
    if(!ExtractOneZipEntry(mv, entries[i], dest_path, password, buffers,
                           mv.total_size, error)) {
      return false;
    }
  }

  return true;
}

static int
RunArchive(const ArchiveConfig &cfg, ArchiveType type, std::string &error) {
  if(type == ARCHIVE_FULL || type == ARCHIVE_UNKNOWN)
    return RunFullArchive(cfg.source, g_status.stage, cfg.password, error);
  if(type == ARCHIVE_RAR) {
    if(cfg.threads == 0)
      return RunFullArchive(cfg.source, g_status.stage, cfg.password, error);
    g_status.effective_threads =
        EffectiveArchiveThreadsForType(cfg.threads, type);
    g_status.total_bytes = 0;
    g_status.total_files = 0;
    WriteStatus(true);
    errno = 0;
    int code = RunUnrarExtract(cfg.source, g_status.stage, cfg.password,
                               g_status.effective_threads);
    if(code != RARX_SUCCESS && code != RARX_WARNING) {
      error = code == RARX_USERBREAK
                  ? "archive: cancelled"
                  : RarExitDiagnostic((RAR_EXIT)code, !cfg.password.empty());
      return code;
    }
    g_status.percent = 100;
    return 0;
  }

  if(type == ARCHIVE_7Z) {
    g_status.effective_threads =
        EffectiveArchiveThreadsForType(cfg.threads, type);
    SevenZArchiveInfo info =
        Probe7zArchive(cfg.source, cfg.password, g_status.effective_threads);
    if(info.code != RARX_SUCCESS && info.code != RARX_WARNING) {
      error = info.reason;
      if(!info.missing_volume.empty()) {
        error += ": ";
        error += info.missing_volume;
      }
      return info.code;
    }

    g_status.total_bytes = info.total_bytes;
    g_status.total_files = info.file_count;
    if(info.file_count > BFPILOT_7Z_MAX_ENTRIES) {
      char buf[160];
      snprintf(buf, sizeof(buf), "7z has too many entries: %u",
               info.file_count);
      error = buf;
      return RARX_FATAL;
    }
    if(info.unknown_size_count > 0) {
      char buf[192];
      snprintf(buf, sizeof(buf), "7z has %u entries without known sizes",
               info.unknown_size_count);
      error = buf;
      return RARX_FATAL;
    }
    if(!CheckDestinationSpace(g_status.stage, info.total_bytes, "7z",
                              error)) {
      return RARX_FATAL;
    }
    if(info.input_bytes > 0 &&
       info.total_bytes >= BFPILOT_7Z_BOMB_MIN_UNPACKED_BYTES &&
       info.total_bytes / info.input_bytes > BFPILOT_7Z_MAX_EXPANSION_RATIO) {
      char buf[256];
      snprintf(buf, sizeof(buf),
               "7z expansion ratio too high: %llu bytes from %llu bytes",
               info.total_bytes, info.input_bytes);
      error = buf;
      return RARX_FATAL;
    }
    WriteStatus(true);
    // Probe/entry/space/expansion checks remain common to both engines.
    if(cfg.threads == 0)
      return RunFullArchive(cfg.source, g_status.stage, cfg.password, error);
    errno = 0;
    SevenZExtractResult result =
        Run7zExtract(cfg.source, g_status.stage, cfg.password,
                     g_status.effective_threads);
    if(result.code != RARX_SUCCESS && result.code != RARX_WARNING) {
      error = result.reason;
      if(!result.missing_volume.empty()) {
        error += ": ";
        error += result.missing_volume;
      }
      return result.code;
    }
    g_status.percent = 100;
    return 0;
  }

  if(type == ARCHIVE_ZIP) {
    g_status.effective_threads = 1;
    errno = 0;
    bool ok = RunZipExtract(cfg.source, g_status.stage, cfg.password,
                            cfg.threads == 0, error);
    return ok ? 0 : error == "archive: cancelled" ? RARX_USERBREAK : RARX_FATAL;
  }

  error = "unsupported archive type";
  return 1;
}

static int FinalizeArchive(const ArchiveConfig &cfg, ArchiveType type, int rc, std::string &error) {
  if(rc == 0) {
    SetStatus("finalizing", "checking and syncing extracted files", true);
    bfpilot_tree_totals_t totals = {};
    int permission_rc = bfpilot_tree_finalize_stats(g_status.stage.c_str(), 0777,
        [](void *) -> int {
          WriteStatus(false);
          return __atomic_load_n(&g_archive_cancel, __ATOMIC_RELAXED);
        }, NULL, &totals);
    if(permission_rc == 0 && g_status.full_engine) {
      g_status.total_bytes = g_status.bytes_written = totals.bytes;
      g_status.total_files = g_status.files_done = totals.files;
    }
    if(permission_rc != 0) {
      errno = -permission_rc;
      error = permission_rc == -ECANCELED ? "archive: cancelled" :
              std::string("archive finalization: ") + strerror(-permission_rc);
      rc = permission_rc == -ECANCELED ? RARX_USERBREAK : RARX_FATAL;
    }
  }
  if(rc != 0) {
    g_status.state = "error";
    g_status.error = error.empty() ? "archive extraction failed" : error;
    g_status.errno_code = errno;
    g_status.archive_exit_code = rc;
    if(cfg.cleanup_partial && g_stage_owned && g_status.helper_quiescent) {
      std::string old_stage = g_status.stage;
      int cleanup_rc = DeleteRecursiveForce(old_stage) ? 0 : -1;
      int cleanup_errno = cleanup_rc == 0 ? 0 : errno;
      LogLine("partial cleanup rc=%d errno=%d stage=%s", cleanup_rc,
              cleanup_errno, old_stage.c_str());
      if(cleanup_rc == 0) {
        g_status.stage.clear();
        g_stage_owned = false;
        g_status.current = "partial extraction cleaned";
      } else {
        g_status.current = "partial extraction cleanup failed";
      }
    }
    ClearActiveFullArchive();
    WriteStatus(true);
    LogLine("extract failed rc=%d errno=%d error=%s stage=%s", rc, errno,
            g_status.error.c_str(), g_status.stage.c_str());
    return 10 + rc;
  }

  SetStatus("finalizing", "renaming staging directory", true);
  if(PathExists(cfg.destination)) {
    g_status.state = "error";
    g_status.error = "destination appeared before finalize";
    g_status.errno_code = EEXIST;
    WriteStatus(true);
    ClearActiveFullArchive();
    LogLine("destination appeared before finalize path=%s",
            cfg.destination.c_str());
    return 20;
  }
  if(rename(g_status.stage.c_str(), cfg.destination.c_str()) != 0) {
    g_status.state = "error";
    g_status.error = "failed to finalize extraction";
    g_status.errno_code = errno;
    WriteStatus(true);
    ClearActiveFullArchive();
    LogLine("finalize failed stage=%s destination=%s errno=%d",
            g_status.stage.c_str(), cfg.destination.c_str(), errno);
    return 21;
  }

  g_status.stage.clear();
  g_stage_owned = false;
  g_status.current = cfg.destination;
  g_status.percent = 100;
  g_status.state = "done";
  WriteStatus(true);
  // Keep the full helper's publication identity until terminal success is
  // durably recorded. Reload can then reconcile this exact batch item.
  if(g_terminal_status_durable) ClearActiveFullArchive();
  else {
    LogLine("published extraction terminal status not durable; recovery identity retained");
    return 21;
  }
  LogLine("extract done type=%s destination=%s bytes_written=%llu files=%llu",
          ArchiveTypeName(type), cfg.destination.c_str(),
          g_status.bytes_written, g_status.files_done);
  return 0;
}

static int
bfpilot_archive_run_prepared_job_impl(void) {
  g_status.request_id.clear();
  g_terminal_status_durable = false;
  g_status.state = "starting";
  g_status.started_ms = NowMs();
  g_status.percent = 0;
  g_status.archive_exit_code = 0;
  g_status.full_engine = false;
  g_status.helper_quiescent = true;
  g_status.helper_task_admitted = false;
  g_status.helper_task_id = 0;
  g_status.decoder_done = g_status.decoder_total = 0;
  g_status.errno_code = 0;
  g_status.priority_nice = 0;
  g_status.priority_rc = 0;
  g_status.priority_errno = 0;
  g_status.total_bytes = 0;
  g_status.bytes_written = 0;
  g_status.files_done = 0;
  g_status.total_files = 0;
  g_status.threads = 0;
  g_status.effective_threads = 0;
  g_status.source.clear();
  g_status.destination.clear();
  g_status.stage.clear();
  g_status.current.clear();
  g_status.error.clear();
  g_status.archive_type.clear();
  g_last_status_ms = 0;
  g_last_progress = -1;
  g_stage_owned = false;
  ResetIoCounters();
  WriteStatus(true);
  LogLine("archive job entry reached");

  ArchiveConfig cfg;
  std::string error;
  if(!LoadConfig(cfg, error)) {
    g_status.request_id = cfg.request_id;
    g_status.state = "error";
    g_status.error = error;
    g_status.errno_code = errno;
    WriteStatus(true);
    LogLine("config error: %s", error.c_str());
    return 2;
  }

  g_status.source = cfg.source;
  g_status.request_id = cfg.request_id;
  g_status.destination = cfg.destination;
  g_status.threads = cfg.threads;
  ApplyArchiveScheduling(cfg.priority_nice);
  WriteStatus(true);

  struct stat src_st;
  if(stat(cfg.source.c_str(), &src_st) != 0 || !S_ISREG(src_st.st_mode)) {
    g_status.state = "error";
    g_status.error = "source archive is not a regular file";
    g_status.errno_code = errno;
    WriteStatus(true);
    LogLine("source stat failed path=%s errno=%d", cfg.source.c_str(), errno);
    return 3;
  }

  if(PathExists(cfg.destination)) {
    g_status.state = "error";
    g_status.error = "destination already exists";
    g_status.errno_code = EEXIST;
    WriteStatus(true);
    LogLine("destination exists path=%s", cfg.destination.c_str());
    return 4;
  }

  std::string parent = DirName(cfg.destination);
  if(!IsDirPath(parent) && !MkdirAll(parent)) {
    g_status.state = "error";
    g_status.error = "failed to create destination parent";
    g_status.errno_code = errno;
    WriteStatus(true);
    LogLine("destination parent failed path=%s errno=%d", parent.c_str(), errno);
    return 5;
  }

  char stage_path[1024];
  if(bfpilot_stage_path(parent.c_str(), "extract", stage_path, sizeof(stage_path)) != 0) {
    g_status.state = "error";
    g_status.error = "failed to select private staging on destination filesystem";
    g_status.errno_code = errno;
    WriteStatus(true); return 6;
  }
  g_status.stage = stage_path;
  if(PathExists(g_status.stage)) {
    g_status.state = "error";
    g_status.error = "staging path already exists";
    g_status.errno_code = EEXIST;
    WriteStatus(true);
    LogLine("stage exists path=%s", g_status.stage.c_str());
    return 6;
  }
  if(mkdir(g_status.stage.c_str(), 0700) != 0) {
    g_status.state = "error";
    g_status.error = "failed to create staging directory";
    g_status.errno_code = errno;
    WriteStatus(true);
    LogLine("stage mkdir failed path=%s errno=%d", g_status.stage.c_str(), errno);
    return 7;
  }

  ArchiveType type = DetectArchiveType(cfg.source);
  g_status.archive_type = ArchiveTypeName(type);
  if(type == ARCHIVE_RAR || type == ARCHIVE_7Z) {
    g_status.effective_threads =
        EffectiveArchiveThreadsForType(cfg.threads, type);
  } else if(type == ARCHIVE_ZIP) {
    g_status.effective_threads = 1;
  }
  g_stage_owned = true;
  SetStatus("running", "extracting", true);
  LogLine("extract start type=%s source=%s destination=%s stage=%s password=%s requested_threads=%u effective_threads=%u mode=%s",
          ArchiveTypeName(type), cfg.source.c_str(), cfg.destination.c_str(),
          g_status.stage.c_str(), cfg.password.empty() ? "empty" : "provided",
          cfg.threads, g_status.effective_threads,
          cfg.threads == 0 ? "auto" : "manual");

  int rc = RunArchive(cfg, type, error);
  return FinalizeArchive(cfg,type,rc,error);
}


extern "C" __attribute__((weak)) std::atomic<int> g_archive_busy(0);

extern "C" int
bfpilot_archive_run_prepared_job(void) {
  int rc = bfpilot_archive_run_prepared_job_impl();
  if(g_status.helper_quiescent && !g_full_marker_pending) g_archive_busy.store(0);
  return rc;
}

static bool
StatusLooksInterrupted(const std::string &status) {
  return status.find("\"state\":\"starting\"") != std::string::npos ||
         status.find("\"state\":\"running\"") != std::string::npos ||
         status.find("\"state\":\"finalizing\"") != std::string::npos ||
         status.find("\"state\":\"prepared\"") != std::string::npos;
}

static void
MarkInterruptedArchiveStatus(const std::string &old_status) {
  g_status.state = "error";
  g_status.started_ms = NowMs();
  g_status.percent = 0;
  g_status.archive_exit_code = RARX_USERBREAK;
  g_status.errno_code = EINTR;
  g_status.priority_nice = 0;
  g_status.priority_rc = 0;
  g_status.priority_errno = 0;
  g_status.total_bytes = 0;
  g_status.bytes_written = 0;
  g_status.files_done = 0;
  g_status.total_files = 0;
  g_status.threads = 0;
  g_status.effective_threads = 0;
  g_status.source.clear();
  g_status.destination.clear();
  g_status.stage.clear();
  g_status.current = "previous archive job interrupted";
  g_status.error =
      "previous archive job was interrupted; inspect logs and any "
      "staging folder recorded in the previous status before cleanup";
  g_status.archive_type = "unknown";
  g_last_status_ms = 0;
  g_last_progress = -1;
  ResetIoCounters();
  WriteStatus(true);

  std::string sample = old_status.substr(0, 1200);
  LogLine("archive interrupted status recovered old_status=%s", sample.c_str());
}

static bool RecoverActiveFullArchive() {
  struct stat marker;
  if(lstat(BFPILOT_FULL_ACTIVE,&marker)!=0)return false;
  g_full_marker_pending=true;
  g_status.started_ms=NowMs();
  g_status.full_engine=true;g_status.helper_quiescent=false;
  g_status.archive_type="auto";g_status.state="recovering";
  ActiveFullArchive active;ArchiveConfig cfg;std::string error;
  if(!LoadActiveFullArchive(active,error) || !LoadConfig(cfg,error) ||
     cfg.source!=active.source || cfg.destination!=active.destination) {
    g_status.error=error.empty()?"active archive configuration changed; retained for inspection":error;
    g_status.state="error";WriteStatus(true);
    LogLine("full archive recovery refused: %s",g_status.error.c_str());return true;
  }
  g_status.source=active.source;g_status.destination=active.destination;
  g_status.request_id=cfg.request_id;
  g_status.stage=active.stage;g_status.helper_task_id=active.id;
  g_status.threads=cfg.threads;g_stage_owned=!active.stage_missing;
  WriteStatus(true);
  int rc=RunFullArchive(active.source,active.stage,cfg.password,error,active.id);
  if(active.stage_missing && rc==0) {
    if(active.published) {
      // The retained helper must confirm success before trusting publication.
      bfpilot_tree_totals_t totals={};
      int sync_rc=bfpilot_tree_finalize_stats(active.destination.c_str(),0777,NULL,NULL,&totals);
      if(!sync_rc) {
        g_status.total_bytes=g_status.bytes_written=totals.bytes;
        g_status.total_files=g_status.files_done=totals.files;
        g_status.state="done";g_status.percent=100;g_status.current=active.destination;
        g_status.stage.clear();WriteStatus(true);
        if(g_terminal_status_durable) ClearActiveFullArchive();
        else rc=21;
      } else {
        g_status.state="error";g_status.error="published archive could not be verified and synced";
        g_status.errno_code=-sync_rc;WriteStatus(true);rc=21;
      }
    } else {
      g_status.state="error";g_status.error="completed archive staging directory is missing; retained for inspection";
      WriteStatus(true);rc=21;
    }
  } else if(active.published) {
    // Never delete or rename a destination through the partial-stage path.
    g_status.state="error";g_status.error="published archive helper result could not be verified: "+error;
    WriteStatus(true);
  } else rc=FinalizeArchive(cfg,ARCHIVE_FULL,rc,error);
  if(g_status.helper_quiescent && !g_full_marker_pending)g_archive_busy.store(0);
  LogLine("full archive recovered task=%llu rc=%d quiet=%d",active.id,rc,g_status.helper_quiescent?1:0);
  return true;
}

static std::atomic<int> g_archive_daemon_exit(0);
#include "archive_batch_runtime.inc"
static std::atomic<int> g_archive_daemon_running(0);
static pthread_mutex_t g_archive_daemon_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_archive_daemon_condition = PTHREAD_COND_INITIALIZER;
static bool g_archive_daemon_pending = false;

extern "C" void
bfpilot_archive_notify_prepared(void) {
  pthread_mutex_lock(&g_archive_daemon_mutex);
  g_archive_daemon_pending = true;
  pthread_cond_signal(&g_archive_daemon_condition);
  pthread_mutex_unlock(&g_archive_daemon_mutex);
}

static void ArchiveDaemonWait(void) {
  // Retain periodic discovery for standalone/legacy file-based submissions.
  // A retained predicate covers a notification arriving before we start waiting.
  struct timespec deadline;
  if(clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    usleep(250000);
    return;
  }
  deadline.tv_nsec += 250000000;
  if(deadline.tv_nsec >= 1000000000) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000;
  }
  pthread_mutex_lock(&g_archive_daemon_mutex);
  while(!g_archive_daemon_pending && !g_archive_daemon_exit.load()) {
    if(pthread_cond_timedwait(&g_archive_daemon_condition,
                             &g_archive_daemon_mutex, &deadline) != 0) break;
  }
  g_archive_daemon_pending = false;
  pthread_mutex_unlock(&g_archive_daemon_mutex);
}

static void *
ArchiveDaemonMain(void *arg) {
  struct RunningGuard { ~RunningGuard() { g_archive_daemon_running=0; } } running_guard;
  if(!MkdirAll(BFPILOT_ARCHIVE_DIR)) {
    LogLine("archive daemon mkdir failed errno=%d", errno);
    return (void *)(intptr_t)2;
  }

  int lock_fd = open(BFPILOT_ARCHIVE_DAEMON_LOCK, O_RDWR | O_CREAT, 0666);
  if(lock_fd < 0) {
    LogLine("archive daemon lock open failed errno=%d", errno);
    return (void *)(intptr_t)3;
  }
  int lock_rc = -1;
  for(int attempt = 0; attempt < 40; attempt++) {
    lock_rc = flock(lock_fd, LOCK_EX | LOCK_NB);
    if(lock_rc == 0) break;
    int saved_errno = errno;
    if(saved_errno != EWOULDBLOCK && saved_errno != EAGAIN) {
      LogLine("archive daemon lock failed errno=%d", saved_errno);
      close(lock_fd);
      return (void *)(intptr_t)4;
    }
    if(attempt == 0) {
      LogLine("archive daemon lock busy; waiting for old daemon");
    }
    usleep(250000);
  }
  if(lock_rc != 0) {
    LogLine("archive daemon lock still busy errno=%d", errno);
    close(lock_fd);
    return (void *)(intptr_t)4;
  }

  char pid_buf[64];
  int pid_len = snprintf(pid_buf, sizeof(pid_buf), "%ld\n", (long)getpid());
  if(pid_len > 0) {
    ftruncate(lock_fd, 0);
    write(lock_fd, pid_buf, (size_t)pid_len);
  }

  LogLine("archive daemon started pid=%ld", (long)getpid());

  const bool recovered=RecoverActiveFullArchive();
  std::string startup_status;
  if(!recovered && ReadSmallFile(BFPILOT_ARCHIVE_STATUS, startup_status, 32768) &&
     StatusLooksInterrupted(startup_status)) {
    MarkInterruptedArchiveStatus(startup_status);
  }
  BatchInitialize();

  std::string last_job;
  for(;;) {
    if(g_archive_daemon_exit) break;
    if(BatchTick()) { ArchiveDaemonWait(); continue; }
    if(bfpilot_archive_batch_busy()) { ArchiveDaemonWait(); continue; }
    std::string status;
    bool prepared = ReadSmallFile(BFPILOT_ARCHIVE_STATUS, status, 32768) &&
                    status.find("\"state\":\"prepared\"") != std::string::npos;
    if(prepared) {
      std::string job;
      if(ReadSmallFile(BFPILOT_ARCHIVE_JOB, job, 32768) && job != last_job && !BatchOwnsJob(job)) {
        last_job = job;
        int rc = bfpilot_archive_run_prepared_job();
        LogLine("archive daemon job finished rc=%d", rc);
      }
    } else {
      last_job.clear();
    }
    ArchiveDaemonWait();
  }
  close(lock_fd);
  return NULL;
}


extern "C" int
bfpilot_archive_start_daemon(void) {
  pthread_mutex_lock(&g_archive_daemon_mutex);
  if(g_archive_daemon_running.exchange(1)) {
    pthread_mutex_unlock(&g_archive_daemon_mutex);
    return -EALREADY;
  }
  g_archive_daemon_exit = 0;
  struct stat active;
  if(lstat(BFPILOT_FULL_ACTIVE,&active)==0)g_archive_busy.store(1);
  pthread_t thread;
  int rc = pthread_create(&thread, NULL, ArchiveDaemonMain, NULL);
  if(rc != 0) {
    g_archive_daemon_running = 0;
    pthread_mutex_unlock(&g_archive_daemon_mutex);
    LogLine("archive daemon thread creation failed rc=%d", rc);
    return -rc;
  }
  pthread_mutex_unlock(&g_archive_daemon_mutex);

  LogLine("archive daemon thread started successfully");

  pthread_detach(thread);
  return 0;
}

extern "C" void
bfpilot_archive_stop_daemon(void) {
  pthread_mutex_lock(&g_archive_daemon_mutex);
  g_archive_daemon_exit = 1;
  pthread_cond_broadcast(&g_archive_daemon_condition);
  pthread_mutex_unlock(&g_archive_daemon_mutex);
  for(int i = 0; i < 50; i++) {
    if(!g_archive_daemon_running) break;
    usleep(20000);
  }
}
