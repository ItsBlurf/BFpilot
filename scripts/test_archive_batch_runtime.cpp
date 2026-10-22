/* Execute the production coordinator with a modeled decoder boundary. Real
   private journal/config I/O, cancellation/reload/order/admission transitions. */
#include "archive_worker.h"
#include "archive_batch_store.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#define BFPILOT_ARCHIVE_DIR "/tmp/bfpilot-batch-runtime-r23"
#define BFPILOT_ARCHIVE_JOB BFPILOT_ARCHIVE_DIR "/job.ini"
#define BFPILOT_ARCHIVE_STATUS BFPILOT_ARCHIVE_DIR "/status.json"
static struct {
  std::string request_id, state;
  bool helper_quiescent = true;
  int errno_code = 0;
} g_status;
static bool g_full_marker_pending, g_terminal_status_durable;
static std::atomic<int> g_archive_busy(0);
static int g_archive_cancel, notify_count, runs, mode, fail_sync;
static unsigned groups;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
  if(fail_sync) { fail_sync = 0; errno = EIO; return -1; }
  return __real_fsync(fd);
}
static void LogLine(const char *format, ...) {
  va_list args; va_start(args, format); vprintf(format, args); va_end(args); puts("");
}
static bool WriteAll(int fd, const void *data, size_t size) {
  const char *p = (const char *)data;
  while(size) { ssize_t n = write(fd, p, size); if(n <= 0) return false; p += n; size -= n; }
  return true;
}
static bool ReadSmallFile(const char *path, std::string &out, size_t maximum) {
  int fd = open(path, O_RDONLY); if(fd < 0) return false;
  char bytes[4096]; out.clear(); ssize_t got;
  while((got = read(fd, bytes, sizeof(bytes))) > 0) {
    out.append(bytes, (size_t)got); if(out.size() > maximum) { close(fd); return false; }
  }
  return close(fd) == 0 && got == 0;
}
static std::string JsonEscape(const std::string &text) {
  std::string out;
  for(char c : text) { if(c == '\\' || c == '"') out += '\\'; out += c; }
  return out;
}
static void terminal(const std::string &id, const char *state) {
  int fd = open(BFPILOT_ARCHIVE_STATUS, O_CREAT | O_TRUNC | O_WRONLY, 0600); assert(fd >= 0);
  std::string text = "{\"requestId\":\"" + id + "\",\"state\":\"" + state + "\"}";
  assert(WriteAll(fd, text.data(), text.size()) && !fsync(fd) && !close(fd));
}
extern "C" void bfpilot_archive_notify_prepared(void) { notify_count++; }
extern "C" int bfpilot_archive_run_prepared_job(void) {
  assert(bfpilot_archive_batch_busy() && g_archive_busy.load()); runs++;
  std::string cfg; assert(ReadSmallFile(BFPILOT_ARCHIVE_JOB, cfg, 32768));
  const std::string key = "\nrequestId="; size_t at = cfg.find(key); assert(at != std::string::npos);
  size_t end = cfg.find('\n', at + key.size()); assert(end != std::string::npos);
  g_status.request_id = cfg.substr(at + key.size(), end - at - key.size());
  assert(cfg.find("\npassword= pwd=中文 \n") != std::string::npos);
  int rc = 0;
  g_status.helper_quiescent = true; g_status.errno_code = 0;
  if(mode == 1 && runs == 2) { rc = 1; g_status.errno_code = EACCES; }
  if(mode == 2) { assert(!bfpilot_archive_batch_cancel()); assert(g_archive_cancel); rc = 1; g_status.errno_code = ECANCELED; }
  if(mode == 3) { g_status.helper_quiescent = false; g_full_marker_pending = true; rc = 1; }
  g_status.state = rc ? "error" : "done";
  terminal(g_status.request_id, g_status.state.c_str()); g_terminal_status_durable = true;
  if(g_status.helper_quiescent && !g_full_marker_pending) g_archive_busy.store(0);
  if(mode == 4) fail_sync = 1;
  return rc;
}
#include "../src/archive_batch_runtime.inc"
static void restart() {
  if(g_batch_directory >= 0) assert(!close(g_batch_directory));
  g_batch_directory = -1; g_batch = {}; g_batch_items.reset(); g_batch_init_error = 0;
  g_batch_ready = g_batch_reconcile = false; g_batch_owner = 1; g_batch_worker_live = 0;
  g_archive_busy = 0; g_status.request_id.clear(); g_status.state.clear();
  g_status.helper_quiescent = true; g_full_marker_pending = false;
  BatchInitialize(); assert(!g_batch_init_error);
}
static void start(const char *id, unsigned count, int next_mode) {
  std::vector<bfab_item_t> items(count);
  bfab_job_t j = {}; strcpy(j.id, id); strcpy(j.password, " pwd=中文 ");
  j.items = items.data(); j.count = count; j.state = BFAB_PREPARED; j.revision = 1;
  for(unsigned i = 0; i < count; i++) {
    snprintf(items[i].source, sizeof(items[i].source), "/data/archive-%u.zip", i);
    snprintf(items[i].destination, sizeof(items[i].destination), "/data/output-%u", i);
  }
  runs = 0; mode = next_mode; g_archive_busy = 1;
  assert(!bfpilot_archive_batch_start(&j)); assert(bfpilot_archive_batch_busy());
}
static void drain() { for(unsigned i = 0; i < 20 && BatchTick(); i++); }
int main() {
  assert(!mkdir(BFPILOT_ARCHIVE_DIR, 0700));
  BatchInitialize(); assert(g_batch_ready && !bfpilot_archive_batch_busy());
  start("mixed", 3, 1); drain();
  assert(runs == 3 && g_batch.succeeded == 2 && g_batch.failed == 1 &&
    g_batch.state == BFAB_FINISHED && !g_archive_busy && !bfpilot_archive_batch_busy());
  assert(BatchOwnsJob("source=/data/a\nrequestId=mixed-2\n"));
  assert(!BatchOwnsJob("source=/data/a\nrequestId=ordinary-single-1\n"));
  char *json = NULL; size_t size = 0;
  assert(!bfpilot_archive_batch_status(&json, &size) && strstr(json, "\"succeeded\":2") &&
    strstr(json, "\"errno\":13") && !strstr(json, "password")); free(json); groups++;
  restart(); assert(g_batch.state == BFAB_FINISHED && !BatchTick() && runs == 3); groups++;
  start("cancel-before", 3, 0); assert(!bfpilot_archive_batch_cancel()); drain();
  assert(!runs && g_batch.state == BFAB_CANCELLED && !g_archive_busy && !g_batch_owner); groups++;
  start("cancel-during", 3, 2); drain();
  assert(runs == 1 && g_batch.state == BFAB_CANCELLED && g_batch.next == 1 &&
    g_batch.items[1].state == BFAB_PENDING && !g_batch_owner); groups++;
  start("cancel-between", 3, 0); assert(BatchTick());
  assert(g_batch_owner && !g_archive_busy && g_batch.next == 1);
  assert(!bfpilot_archive_batch_cancel()); drain();
  assert(runs == 1 && g_batch.state == BFAB_CANCELLED); groups++;
  start("pending-reload", 3, 0); restart(); drain();
  assert(runs == 3 && g_batch.succeeded == 3); groups++;
  start("summary-protection", 3, 0);
  assert(bfpilot_archive_batch_protect_path("/data/archive-2.zip",NULL)==-EBUSY);
  assert(!bfpilot_archive_batch_protect_path("/data/unrelated.zip",NULL));
  assert(!bfpilot_archive_batch_summary(&json,&size) && strstr(json,"\"count\":3") &&
    strstr(json,"archive-0.zip") && !strstr(json,"archive-1.zip"));free(json);
  assert(!bfpilot_archive_batch_cancel());
  assert(!bfpilot_archive_batch_protect_path("/data/archive-2.zip",NULL));groups++;
  start("completed-reload", 2, 0); assert(!BatchEvent(BFAB_BEGIN));
  std::string expected = BatchRequestId(); terminal(expected, "done");
  restart(); assert(BatchTick());
  assert(!runs && g_batch.next == 1 && g_batch.succeeded == 1);
  drain(); assert(runs == 1 && g_batch.succeeded == 2); groups++;
  start("foreign-status", 2, 0); assert(!BatchEvent(BFAB_BEGIN)); terminal("older-same-path", "done");
  restart(); drain(); assert(!runs && g_batch.state == BFAB_ERROR && g_batch.next == 0); groups++;
  start("uncertain-status", 2, 0); assert(!BatchEvent(BFAB_BEGIN)); terminal(BatchRequestId(), "prepared");
  restart(); drain(); assert(!runs && g_batch.state == BFAB_ERROR && g_batch.next == 0); groups++;
  start("uncertain-append", 2, 4); assert(BatchTick());
  assert(runs == 1 && g_batch.state == BFAB_ERROR && g_batch.next == 0);
  mode = 0; restart(); assert(g_batch.next == 1); drain();
  assert(runs == 2 && g_batch.succeeded == 2); groups++;
  start("helper-not-quiet", 2, 3); drain();
  assert(runs == 1 && g_batch.state == BFAB_ERROR && g_batch_owner && g_archive_busy);
  restart(); // modeled helper has now stopped; retained error never redispatches.
  assert(!BatchTick() && runs == 1 && g_batch.state == BFAB_ERROR); groups++;
  assert(notify_count > 0); assert(!close(g_batch_directory));
  DIR *scan = opendir(BFPILOT_ARCHIVE_DIR "/batch"); assert(scan); struct dirent *entry;
  while((entry = readdir(scan))) if(strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
    assert(!unlinkat(dirfd(scan), entry->d_name, 0));
  assert(!closedir(scan) && !rmdir(BFPILOT_ARCHIVE_DIR "/batch"));
  assert(!unlink(BFPILOT_ARCHIVE_JOB) && !unlink(BFPILOT_ARCHIVE_STATUS));
  assert(!rmdir(BFPILOT_ARCHIVE_DIR));
  printf("archive batch coordinator: %u production control groups passed; decoder boundary modeled\n", groups);
}
