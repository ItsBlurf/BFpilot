/* Included after the shared download/TAR framing helpers. Only mixed/remote
 * downloads use this endpoint path; local sequential fast paths are retained. */
struct remote_stream {
  const http_request_t *request;
  bfpilot_io_t *endpoint;
  unsigned char *buffer;
  size_t capacity;
  const char *root_name;
  int counted, chunked;
  bfpilot_download_stats_t stats;
};

static int remote_stream_file(struct remote_stream *stream, const char *path,
                               const struct stat *expected, off_t offset) {
  bfpilot_io_file_t *file = NULL;
  struct stat opened;
  int rc = bfpilot_io_open(stream->endpoint, path, O_RDONLY, 0, &file);
  if (!rc) rc = bfpilot_io_fstat(file, &opened);
  if (!rc && !remote_identity(expected, &opened, 1)) rc = -ESTALE;
  if (!rc && offset) rc = bfpilot_io_seek(file, offset);
  unsigned long long left = (unsigned long long)(expected->st_size - offset);
  while (!rc && left) {
    if (stream->counted && job_cancelled()) { rc = -ECANCELED; break; }
    size_t want = left > stream->capacity ? stream->capacity : (size_t)left;
    long start = monotonic_ms();
    ssize_t got = bfpilot_io_read(file, stream->buffer, want);
    stream->stats.read_ms += monotonic_ms() - start;
    if (got <= 0) { rc = got < 0 ? (int)got : -EIO; break; }
    if (stream->counted) atomic_fetch_add(&g_job.read_bytes, (long)got);
    start = monotonic_ms();
    int sent = stream->chunked
        ? websrv_write_chunk(stream->request->fd, stream->buffer, (size_t)got)
        : websrv_write_all(stream->request->fd, stream->buffer, (size_t)got);
    stream->stats.send_ms += monotonic_ms() - start;
    if (sent) { rc = -(errno ? errno : EPIPE); break; }
    left -= (unsigned long long)got;
    stream->stats.bytes += (unsigned long)got;
    if (stream->counted) {
      atomic_fetch_add(&g_job.written_bytes, (long)got);
      atomic_fetch_add(&g_job.copied_bytes, (long)got);
    }
  }
  if (!rc) {
    rc = bfpilot_io_fstat(file, &opened);
    if (!rc && !remote_identity(expected, &opened, 1)) rc = -EAGAIN;
  }
  if (file) {
    int closed = bfpilot_io_close(file);
    if (!rc) rc = closed;
  }
  if (!rc && stream->counted) atomic_fetch_add(&g_job.done_files, 1);
  if (rc == -EAGAIN || rc == -ESTALE)
    if (stream->counted) job_set_error("source changed during download: %s", path);
  return rc;
}

/* Preview and log-tail requests do not acquire or overwrite the job owner's
 * counters. They still consume one of the bounded SMB contexts. */
int transfer_remote_file_request(const http_request_t *req, const char *path) {
  struct remote_stream stream = {0};
  stream.request = req;
  stream.capacity = BFPILOT_DOWNLOAD_BUF_SIZE;
  struct stat st;
  int rc = bfpilot_io_endpoint(path, &stream.endpoint);
  if (!rc) rc = bfpilot_io_lstat(stream.endpoint, path, &st);
  if (!rc && (!S_ISREG(st.st_mode) || st.st_size < 0)) rc = -EINVAL;
  off_t offset = 0;
  char text[32];
  if (!rc && websrv_get_query_arg(req, "tail", text, sizeof(text))) {
    char *end;
    errno = 0;
    unsigned long long tail = strtoull(text, &end, 10);
    if (errno || end == text || *end) rc = -EINVAL;
    else if (tail && tail < (unsigned long long)st.st_size)
      offset = st.st_size - (off_t)tail;
  }
  int aligned = 0;
  if (!rc) {
    stream.buffer = alloc_aligned_io_buffer(stream.capacity, &aligned);
    if (!stream.buffer) rc = -ENOMEM;
  }
  long start = monotonic_ms();
  if (rc) {
    bfpilot_io_end(stream.endpoint);
    free(stream.buffer);
    return serve_error(req, rc == -ENOENT ? 404 : rc == -EBUSY ? 409 : 400,
                       strerror(-rc));
  }
  websrv_conn_set_keep_alive(0);
  const char *mime = mime_get_type(path);
  if (websrv_send_headers(req->fd, 200, mime ? mime : "application/octet-stream",
                          (size_t)(st.st_size - offset), NULL))
    rc = -(errno ? errno : EPIPE);
  if (!rc) rc = remote_stream_file(&stream, path, &st, offset);
  stream.stats.elapsed_ms = monotonic_ms() - start;
  stream.stats.buffer_size = stream.capacity;
  stream.stats.buffer_aligned = aligned;
  stream.stats.error_code = rc ? -rc : 0;
  snprintf(stream.stats.path, sizeof(stream.stats.path), "%s", path);
  bfpilot_fs_record_download_stats(&stream.stats);
  bfpilot_io_end(stream.endpoint);
  free(stream.buffer);
  if (rc) errno = -rc;
  return rc ? -1 : 0;
}

static int remote_tar_entry(const char *path, const char *relative,
                              const struct stat *st, void *context) {
  struct remote_stream *stream = context;
  char name[2048];
  int length = snprintf(name, sizeof(name), "%s%s%s", stream->root_name,
                         *relative ? "/" : "", relative);
  if (length < 0 || (size_t)length >= sizeof(name)) return -ENAMETOOLONG;
  if (st->st_size < 0 || st->st_size > LONG_MAX - atomic_load(&g_job.total_bytes) ||
      atomic_load(&g_job.total_files) == INT_MAX) return -EOVERFLOW;
  atomic_fetch_add(&g_job.total_files, 1);
  job_set_current(path);
  if (S_ISDIR(st->st_mode)) {
    if (tar_send_header(stream->request, name, st, '5', 0))
      return -(errno ? errno : EPIPE);
    atomic_fetch_add(&g_job.done_files, 1);
    return 0;
  }
  atomic_fetch_add(&g_job.total_bytes, (long)st->st_size);
  if (tar_send_header(stream->request, name, st, '0', st->st_size))
    return -(errno ? errno : EPIPE);
  int rc = remote_stream_file(stream, path, st, 0);
  if (!rc && tar_send_zeros(stream->request, (512 - (st->st_size % 512)) % 512))
    rc = -(errno ? errno : EPIPE);
  return rc;
}

static int remote_download_request(const http_request_t *req, char **paths,
                                     size_t count, unsigned char *buffer) {
  struct remote_stream stream = {0};
  stream.request = req;
  stream.buffer = buffer;
  stream.capacity = BFPILOT_DOWNLOAD_BUF_SIZE;
  stream.counted = 1;
  struct stat first;
  int rc = bfpilot_io_endpoint(paths[0], &stream.endpoint);
  if (!rc) rc = bfpilot_io_lstat(stream.endpoint, paths[0], &first);
  if (rc) goto done;
  websrv_conn_set_keep_alive(0);
  char extra[1200];
  if (count == 1 && S_ISREG(first.st_mode)) {
    if (first.st_size < 0 || first.st_size > LONG_MAX) { rc = -EOVERFLOW; goto done; }
    download_header_filename(path_basename(paths[0]), extra, sizeof(extra));
    atomic_store(&g_job.total_bytes, (long)first.st_size);
    atomic_store(&g_job.total_files, 1);
    job_set_current(paths[0]);
    if (websrv_send_headers(req->fd, 200, "application/octet-stream",
                            (size_t)first.st_size, extra)) rc = -(errno ? errno : EPIPE);
    if (!rc) rc = remote_stream_file(&stream, paths[0], &first, 0);
    goto done;
  }
  bfpilot_io_end(stream.endpoint);
  stream.endpoint = NULL;
  char **names = download_root_names(paths, count);
  if (!names) { rc = -ENOMEM; goto done; }
  char archive[320];
  if (count == 1) snprintf(archive, sizeof(archive), "%.*s.tar", 280, names[0]);
  else snprintf(archive, sizeof(archive), "bfpilot-download.tar");
  download_header_filename(archive, extra, sizeof(extra));
  stream.chunked = 1;
  atomic_store(&g_job.total_known, 0);
  if (websrv_send_chunked_headers(req->fd, 200, "application/x-tar", extra))
    rc = -(errno ? errno : EPIPE);
  for (size_t i = 0; !rc && i < count; i++) {
    stream.root_name = names[i];
    rc = bfpilot_io_endpoint(paths[i], &stream.endpoint);
    if (!rc) rc = bfpilot_io_walk(stream.endpoint, paths[i], 0,
                                  remote_tar_entry, remote_cancelled, &stream);
    bfpilot_io_end(stream.endpoint);
    stream.endpoint = NULL;
  }
  if (!rc && (tar_send_zeros(req, 1024) || websrv_write_chunk_end(req->fd)))
    rc = -(errno ? errno : EPIPE);
  download_free_paths(names, count);
done:
  bfpilot_io_end(stream.endpoint);
  if (rc) errno = -rc;
  return rc ? -1 : 0;
}
