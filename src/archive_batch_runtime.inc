/* Included by archive_worker.cpp after its recovery helpers. The daemon is the
   sole dispatcher; HTTP threads only publish/cancel/snapshot under this lock. */
static pthread_mutex_t g_batch_lock = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<int> g_batch_owner(1);
static std::atomic<int> g_batch_safe_to_release(0), g_batch_worker_live(0);
static bfab_job_t g_batch = {};
static std::unique_ptr<bfab_item_t[]> g_batch_items;
static int g_batch_directory = -1, g_batch_init_error = 0;
static bool g_batch_ready = false, g_batch_reconcile = false;
struct BatchLock {
  BatchLock() { pthread_mutex_lock(&g_batch_lock); }
  ~BatchLock() { pthread_mutex_unlock(&g_batch_lock); }
};
static bool BatchActive() {
  return g_batch.count && (g_batch.state == BFAB_PREPARED || g_batch.state == BFAB_RUNNING);
}
static std::string BatchRequestId() {
  char index[24]; snprintf(index, sizeof(index), "-%u", g_batch.next);
  return std::string(g_batch.id) + index;
}
static bool BatchOwnsJob(const std::string &job) {
  BatchLock lock;
  return g_batch.count && job.find(std::string("\nrequestId=") + g_batch.id + "-") != std::string::npos;
}
extern "C" int bfpilot_archive_batch_protect_path(const char *path, const struct stat *existing) {
  (void)existing;
  BatchLock lock;
  if(!BatchActive()) return 0;
  for(unsigned i = g_batch.next; i < g_batch.count; i++)
    if(!strcmp(path,g_batch.items[i].source)) return -EBUSY;
  return 0;
}
static int BatchEvent(unsigned kind, int result = 0) {
  bfab_event_t e = {};
  snprintf(e.id, sizeof(e.id), "%s", g_batch.id);
  e.sequence = g_batch.revision + 1;
  e.kind = kind;
  e.index = kind == BFAB_CANCEL || kind == BFAB_HALT ? UINT32_MAX : g_batch.next;
  e.result = result;
  int rc = bfab_store_append(g_batch_directory, &g_batch, &e);
  if(rc) {
    // Never retry an uncertain append or dispatch using the uncommitted state.
    g_batch.state = BFAB_ERROR; g_batch.error = -rc;
    LogLine("archive batch journal failed id=%s kind=%u rc=%d; dispatch stopped",
            g_batch.id, kind, rc);
  }
  if(!BatchActive() && g_batch_safe_to_release.load())
    g_batch_owner.store(0);
  return rc;
}
static void BatchHalt(int error) {
  (void)BatchEvent(BFAB_HALT, error ? error : EIO);
  LogLine("archive batch halted id=%s index=%u error=%d; current stage retained for inspection",
          g_batch.id, g_batch.next, g_batch.error);
}
static void BatchInitialize() {
  BatchLock lock;
  g_batch_items.reset(new(std::nothrow) bfab_item_t[BFAB_MAX_ITEMS]);
  if(!g_batch_items) g_batch_init_error = -ENOMEM;
  else {
    g_batch_directory = bfab_store_open(BFPILOT_ARCHIVE_DIR "/batch");
    if(g_batch_directory < 0) g_batch_init_error = g_batch_directory;
    else {
      unsigned torn = 0;
      int rc = bfab_store_load(g_batch_directory, &g_batch,
                               g_batch_items.get(), BFAB_MAX_ITEMS, &torn);
      if(rc != -ENOENT && rc) g_batch_init_error = rc;
      if(torn) LogLine("archive batch recovered short journal tail bytes=%u", torn);
    }
  }
  g_batch_ready = true;
  g_batch_safe_to_release.store(g_status.helper_quiescent && !g_full_marker_pending ? 1 : 0);
  g_batch_reconcile = BatchActive() && g_batch.next < g_batch.count &&
      g_batch.items[g_batch.next].state == BFAB_ACTIVE;
  g_batch_owner.store(g_batch_init_error || BatchActive() ? 1 : 0);
  if(g_batch_init_error)
    LogLine("archive batch recovery blocked rc=%d; private files retained", g_batch_init_error);
}
extern "C" int bfpilot_archive_batch_busy(void) { return g_batch_owner.load(); }
extern "C" int bfpilot_archive_batch_start(const bfab_job_t *input) {
  BatchLock lock;
  if(!g_batch_ready) return -EAGAIN;
  if(g_batch_init_error) return g_batch_init_error;
  if(BatchActive() || g_batch_worker_live.load() || !g_batch_safe_to_release.load()) return -EBUSY;
  if(bfab_validate(input) || input->state != BFAB_PREPARED || input->revision != 1)
    return -EINVAL;
  int rc = bfab_store_publish(g_batch_directory, input);
  if(rc) return rc;
  char previous[BFAB_ID_SIZE]; snprintf(previous, sizeof(previous), "%s", g_batch.id);
  memcpy(g_batch_items.get(), input->items, input->count * sizeof(bfab_item_t));
  g_batch = *input; g_batch.items = g_batch_items.get();
  g_batch_reconcile = false;
  g_batch_owner.store(1);
  if(previous[0] && strcmp(previous, g_batch.id)) {
    int cleanup = bfab_store_retire(g_batch_directory, previous);
    if(cleanup) LogLine("archive previous queue journal retained rc=%d", cleanup);
  }
  bfpilot_archive_notify_prepared();
  return 0;
}
extern "C" int bfpilot_archive_batch_cancel(void) {
  BatchLock lock;
  if(!BatchActive()) return 0;
  int rc = (g_batch.flags & BFAB_CANCEL_REQUESTED) ? 0 : BatchEvent(BFAB_CANCEL);
  if(!BatchActive() && !g_batch_worker_live.load() && g_batch_safe_to_release.load())
    g_archive_busy.store(0);
  // Even a journal I/O error must request that the current decoder stop.
  __atomic_store_n(&g_archive_cancel, 1, __ATOMIC_RELAXED);
  bfpilot_archive_notify_prepared();
  return rc;
}
static const char *BatchStateName(unsigned state) {
  switch(state) {
  case BFAB_PREPARED: return "prepared";
  case BFAB_RUNNING: return "running";
  case BFAB_FINISHED: return "done";
  case BFAB_CANCELLED: return "cancelled";
  case BFAB_ERROR: return "error";
  default: return "idle";
  }
}
static int BatchStatus(char **out, size_t *size, bool summary) {
  if(!out || !size) return -EINVAL;
  *out = NULL; *size = 0;
  BatchLock lock;
  char fields[512];
  snprintf(fields, sizeof(fields),
      "{\"ok\":true,\"state\":\"%s\",\"id\":\"%s\",\"count\":%u,\"next\":%u,"
      "\"succeeded\":%u,\"failed\":%u,\"cancelRequested\":%s,\"errno\":%d,\"items\":[",
      !g_batch_ready ? "recovering" : g_batch_init_error ? "error" : BatchStateName(g_batch.state),
      g_batch.id, g_batch.count, g_batch.next, g_batch.succeeded, g_batch.failed,
      (g_batch.flags & BFAB_CANCEL_REQUESTED) ? "true" : "false",
      g_batch_init_error ? -g_batch_init_error : g_batch.error);
  std::string body = fields;
  for(unsigned i = 0; i < g_batch.count; i++) {
    if(summary && i != g_batch.next) continue;
    const bfab_item_t &it = g_batch.items[i];
    if(!summary && i) body += ',';
    body += "{\"source\":\"" + JsonEscape(it.source) + "\",\"destination\":\"" +
            JsonEscape(it.destination) + "\",\"state\":\"" +
            (it.state == BFAB_DONE ? "done" : it.state == BFAB_FAILED ? "error" :
             it.state == BFAB_ACTIVE ? "running" : "pending") + "\",\"errno\":";
    char result[32]; snprintf(result, sizeof(result), "%d}", it.result); body += result;
  }
  body += "]}";
  char *copy = (char *)malloc(body.size() + 1);
  if(!copy) return -ENOMEM;
  memcpy(copy, body.c_str(), body.size() + 1); *out = copy; *size = body.size();
  return 0;
}
extern "C" int bfpilot_archive_batch_status(char **out, size_t *size) { return BatchStatus(out,size,false); }
extern "C" int bfpilot_archive_batch_summary(char **out, size_t *size) { return BatchStatus(out,size,true); }
static int BatchPublishJob() {
  const bfab_item_t &item = g_batch.items[g_batch.next];
  char options[160];
  snprintf(options, sizeof(options), "\nthreads=%u\nnice=%d\ncleanupPartial=%u\nmerge=%u\n",
           g_batch.threads, g_batch.nice, (g_batch.flags & BFAB_CLEANUP) ? 1 : 0,
           (g_batch.flags & BFAB_MERGE) ? 1 : 0);
  std::string text = std::string("source=") + item.source + "\ndestination=" +
      item.destination + "\npassword=" + g_batch.password + options +
      "requestId=" + BatchRequestId() + "\n";
  const char *temporary = BFPILOT_ARCHIVE_JOB ".tmp";
  int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if(fd < 0) return -errno;
  int rc = WriteAll(fd, text.data(), text.size()) ? 0 : -(errno ? errno : EIO);
  if(!rc && fsync(fd)) rc = -errno;
  if(close(fd) && !rc) rc = -errno;
  if(!rc && rename(temporary, BFPILOT_ARCHIVE_JOB)) rc = -errno;
  if(rc) unlink(temporary);
  return rc;
}
static bool BatchTick() {
  pthread_mutex_lock(&g_batch_lock);
  if(!g_batch_ready || g_batch_init_error || !BatchActive()) {
    pthread_mutex_unlock(&g_batch_lock); return false;
  }
  if(g_batch_reconcile) {
    g_batch_reconcile = false;
    std::string status;
    const std::string expected = "\"requestId\":\"" + BatchRequestId() + "\"";
    if(g_status.helper_quiescent && !g_full_marker_pending &&
       ReadSmallFile(BFPILOT_ARCHIVE_STATUS, status, 32768) &&
       status.find(expected) != std::string::npos &&
       status.find("\"state\":\"done\"") != std::string::npos) {
      (void)BatchEvent(BFAB_FINISH);
    } else {
      // Unknown/manual interruption or an unconfirmed publication is never
      // extracted a second time merely because its source/destination match.
      BatchHalt(EINTR);
    }
    pthread_mutex_unlock(&g_batch_lock); return true;
  }
  if(g_batch.items[g_batch.next].state != BFAB_PENDING) {
    BatchHalt(EPROTO); pthread_mutex_unlock(&g_batch_lock); return true;
  }
  __atomic_store_n(&g_archive_cancel, 0, __ATOMIC_RELAXED);
  int rc = BatchEvent(BFAB_BEGIN);
  if(!rc) rc = BatchPublishJob();
  if(rc) {
    if(g_batch.state != BFAB_ERROR) BatchHalt(-rc);
    if(g_status.helper_quiescent && !g_full_marker_pending) g_archive_busy.store(0);
    pthread_mutex_unlock(&g_batch_lock); return true;
  }
  const std::string request = BatchRequestId();
  g_archive_busy.store(1);
  g_batch_worker_live.store(1); g_batch_safe_to_release.store(0);
  pthread_mutex_unlock(&g_batch_lock);
  rc = bfpilot_archive_run_prepared_job();
  pthread_mutex_lock(&g_batch_lock);
  g_batch_worker_live.store(0);
  g_batch_safe_to_release.store(g_status.helper_quiescent && !g_full_marker_pending ? 1 : 0);
  if(g_batch.state != BFAB_ERROR) {
    if(!g_status.helper_quiescent || g_full_marker_pending ||
       !g_terminal_status_durable || g_status.request_id != request ||
       (g_status.state != "done" && g_status.state != "error")) BatchHalt(EIO);
    else (void)BatchEvent(BFAB_FINISH, rc ?
        (g_status.errno_code ? g_status.errno_code : EIO) : 0);
  }
  if(!BatchActive() && g_batch_safe_to_release.load()) g_batch_owner.store(0);
  pthread_mutex_unlock(&g_batch_lock);
  // A ready next item does not need to wait for the legacy 250 ms poll.
  bfpilot_archive_notify_prepared();
  return true;
}
