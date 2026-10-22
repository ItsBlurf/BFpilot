/* Remote uploads share the job lease and bounded single-buffer receive loop.
 * A server FLUSH and checked CLOSE precede same-share atomic publication. */
static int remote_upload_request(const http_request_t *req, const char *dir,
                                  const char *final, const char *initial,
                                  size_t initial_size, size_t content_size) {
  websrv_conn_set_keep_alive(0);
  if (content_size > LONG_MAX)
    return serve_error(req, 413, "upload is too large");
  if (!job_begin("upload"))
    return serve_error(req, 409, "another file-manager job is running");
  pthread_mutex_lock(&g_job.lock);
  snprintf(g_job.source, sizeof(g_job.source), "%s", path_basename(final));
  snprintf(g_job.destination, sizeof(g_job.destination), "%s", final);
  pthread_mutex_unlock(&g_job.lock);
  job_set_current(final);
  atomic_store(&g_job.total_files, 1);
  atomic_store(&g_job.total_bytes, (long)content_size);
  bfpilot_io_t *endpoint = NULL;
  bfpilot_io_file_t *file = NULL;
  struct stat old, current;
  int exists = 0, owned = 0, aligned = 0;
  char stage[1024] = {0};
  unsigned char *buffer = NULL;
  size_t remaining = content_size, bytes = 0;
  long started = monotonic_ms(), recv_ms = 0, write_ms = 0, sync_ms = 0;
  unsigned int sync_count = 0;
  unsigned long long since_sync = 0;
  int rc = bfpilot_io_endpoint(final, &endpoint);
  if (!rc) rc = bfpilot_io_mkdirs(endpoint, dir, 0777);
  if (!rc) {
    int got = bfpilot_io_lstat(endpoint, final, &old);
    if (!got) { exists = 1; if (!S_ISREG(old.st_mode)) rc = -EEXIST; }
    else if (got != -ENOENT) rc = got;
  }
  if (!rc) {
    struct remote_copy staging = {0};
    staging.destination = endpoint;
    staging.source_path = final;
    rc = remote_stage(&staging, dir, stage, sizeof(stage));
  }
  if (!rc) {
    rc = bfpilot_io_open(endpoint, stage, O_WRONLY | O_CREAT | O_EXCL, 0600, &file);
    if (!rc) owned = 1;
  }
  if (!rc) {
    buffer = alloc_aligned_io_buffer(UPLOAD_BUF_SIZE, &aligned);
    if (!buffer) rc = -ENOMEM;
  }
  struct timeval timeout = {8, 0};
  (void)setsockopt(req->fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  if (initial_size > remaining) initial_size = remaining;
  while (!rc && remaining) {
    if (job_cancelled()) { rc = -ECANCELED; break; }
    const unsigned char *data = buffer;
    size_t size;
    if (initial_size) {
      data = (const unsigned char *)initial;
      size = initial_size;
      initial_size = 0;
    } else {
      size_t want = remaining < UPLOAD_BUF_SIZE ? remaining : UPLOAD_BUF_SIZE;
      long start = monotonic_ms();
      ssize_t got = recv(req->fd, buffer, want, 0);
      recv_ms += monotonic_ms() - start;
      if (got < 0 && errno == EINTR) continue;
      if (got <= 0) {
        rc = job_cancelled() ? -ECANCELED :
             got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? -ETIMEDOUT :
             -(got < 0 ? errno : ECONNRESET);
        break;
      }
      size = (size_t)got;
    }
    remaining -= size;
    atomic_fetch_add(&g_job.read_bytes, (long)size);
    size_t offset = 0;
    long start = monotonic_ms();
    while (!rc && offset < size) {
      if (job_cancelled()) { rc = -ECANCELED; break; }
      ssize_t got = bfpilot_io_write(file, data + offset, size - offset);
      if (got <= 0) { rc = got < 0 ? (int)got : -EIO; break; }
      offset += (size_t)got;
      bytes += (size_t)got;
      since_sync += (unsigned long long)got;
      atomic_fetch_add(&g_job.written_bytes, (long)got);
      atomic_fetch_add(&g_job.copied_bytes, (long)got);
    }
    write_ms += monotonic_ms() - start;
    if (!rc && since_sync >= BFPILOT_WRITEBACK_SYNC_BYTES) {
      start = monotonic_ms();
      rc = bfpilot_io_sync(file);
      sync_ms += monotonic_ms() - start;
      if (!rc) { sync_count++; since_sync = 0; }
    }
  }
  if (!rc && job_cancelled()) rc = -ECANCELED;
  if (!rc) {
    long start = monotonic_ms();
    rc = bfpilot_io_sync(file);
    sync_ms += monotonic_ms() - start;
    if (!rc) sync_count++;
  }
  if (file) { int closed = bfpilot_io_close(file); if (!rc) rc = closed; }
  if (!rc) {
    int got = bfpilot_io_lstat(endpoint, final, &current);
    if (!exists && !got) rc = -EEXIST;
    else if (exists && (got == -ENOENT || (!got && !remote_identity(&old, &current, 1))))
      rc = -EAGAIN;
    else if (got && got != -ENOENT) rc = got;
    if (!rc) rc = bfpilot_io_rename(endpoint, stage, final, exists);
    if (!rc) owned = 0;
  }
  if (owned) {
    int cleaned = bfpilot_io_remove(endpoint, stage, 0);
    if (cleaned) bfpilot_log("SMB upload cleanup rc=%d retained=%s", cleaned, stage);
    if (!rc && cleaned) rc = cleaned;
  }
  bfpilot_io_end(endpoint);
  free(buffer);
  long elapsed = monotonic_ms() - started;
  pthread_mutex_lock(&g_upload.lock);
  g_upload.elapsed_ms = elapsed;
  g_upload.bytes = bytes;
  g_upload.destination_dev = 0;
  g_upload.error_code = rc ? -rc : 0;
  g_upload.recv_ms = recv_ms;
  g_upload.write_ms = write_ms;
  g_upload.sync_ms = sync_ms;
  g_upload.sync_count = sync_count;
  g_upload.buffer_aligned = aligned;
  snprintf(g_upload.path, sizeof(g_upload.path), "%s", final);
  pthread_mutex_unlock(&g_upload.lock);
  atomic_store(&g_job.writeback_syncs, sync_count);
  if (rc) {
    errno = -rc;
    const char *message = rc == -ECANCELED ? "upload cancelled" :
                          rc == -ECONNRESET ? "short upload" :
                          rc == -EAGAIN ? "upload destination changed during transfer" : strerror(-rc);
    job_end(-1, message);
    return serve_error(req, rc == -EAGAIN || rc == -EEXIST ? 409 : 500, message);
  }
  atomic_store(&g_job.done_files, 1);
  bfpilot_search_mark_stale("network upload");
  job_end(0, NULL);
  json_buf_t response = {0};
  if (json_append(&response, "{\"ok\":true,\"remote\":true,\"path\":") ||
      json_string(&response, final) ||
      json_appendf(&response, ",\"size\":%lu,\"elapsedMs\":%ld,\"averageMBps\":%.2f,"
                   "\"recvMs\":%ld,\"writeMs\":%ld,\"syncMs\":%ld,\"syncCount\":%u,"
                   "\"destinationDev\":0,\"completion\":\"server-flush-close-rename\"}",
                   (unsigned long)bytes, elapsed,
                   elapsed > 0 ? (double)bytes * 1000 / elapsed / (1024 * 1024) : 0,
                   recv_ms, write_ms, sync_ms, sync_count)) {
    free(response.data);
    return -1;
  }
  return serve_owned(req, 200, response.data, response.len);
}
