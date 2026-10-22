/* Bounded UTF8 text read and optimistic, same-share staged replacement. The
 * common text API retains its extension, newline, version and size policies. */
static int remote_text_read(const char *path, char **data_out, size_t *size_out,
                              mode_t *mode_out, struct stat *identity_out) {
  bfpilot_io_t *endpoint = NULL;
  bfpilot_io_file_t *file = NULL;
  struct stat expected, opened;
  char *data = NULL;
  int rc = bfpilot_io_endpoint(path, &endpoint);
  if (!rc) rc = bfpilot_io_lstat(endpoint, path, &expected);
  if (!rc && !S_ISREG(expected.st_mode)) rc = -EINVAL;
  if (!rc && (expected.st_size < 0 || expected.st_size > BFPILOT_TEXT_MAX_SIZE))
    rc = -EFBIG;
  if (!rc) rc = bfpilot_io_open(endpoint, path, O_RDONLY, 0, &file);
  if (!rc) rc = bfpilot_io_fstat(file, &opened);
  if (!rc && !remote_identity(&expected, &opened, 1)) rc = -EAGAIN;
  size_t used = 0;
  if (!rc) { data = malloc((size_t)expected.st_size + 1); if (!data) rc = -ENOMEM; }
  while (!rc && used < (size_t)expected.st_size) {
    ssize_t got = bfpilot_io_read(file, data + used, (size_t)expected.st_size - used);
    if (got <= 0) { rc = got < 0 ? (int)got : -EAGAIN; break; }
    used += (size_t)got;
  }
  if (!rc) {
    char extra;
    ssize_t got = bfpilot_io_read(file, &extra, 1);
    if (got) rc = got < 0 ? (int)got : -EAGAIN;
  }
  if (!rc) rc = bfpilot_io_fstat(file, &opened);
  if (!rc && !remote_identity(&expected, &opened, 1)) rc = -EAGAIN;
  if (file) { int closed = bfpilot_io_close(file); if (!rc) rc = closed; }
  bfpilot_io_end(endpoint);
  if (rc) { free(data); errno = -rc; return -1; }
  data[used] = 0;
  *data_out = data;
  *size_out = used;
  if (mode_out) *mode_out = expected.st_mode;
  if (identity_out) *identity_out = expected;
  return 0;
}

static int remote_text_write(const char *path, const char *data, size_t size,
                               const struct stat *expected) {
  bfpilot_io_t *endpoint = NULL;
  bfpilot_io_file_t *file = NULL;
  char parent[1024], stage[1024] = {0};
  snprintf(parent, sizeof(parent), "%s", path);
  char *slash = strrchr(parent, '/');
  if (!slash || slash == parent) { errno = EINVAL; return -1; }
  *slash = 0;
  int owned = 0;
  struct stat current;
  int rc = bfpilot_io_endpoint(path, &endpoint);
  if (!rc) {
    int got = bfpilot_io_lstat(endpoint, path, &current);
    if (expected) {
      if (got) rc = got;
      else if (!remote_identity(expected, &current, 1)) rc = -EAGAIN;
    } else if (!got) rc = -EEXIST;
    else if (got != -ENOENT) rc = got;
  }
  if (!rc) {
    struct stat directory;
    rc = bfpilot_io_lstat(endpoint, parent, &directory);
    if (!rc && !S_ISDIR(directory.st_mode)) rc = -ENOTDIR;
  }
  if (!rc) {
    struct remote_copy staging = {0};
    staging.destination = endpoint;
    staging.source_path = path;
    rc = remote_stage(&staging, parent, stage, sizeof(stage));
  }
  if (!rc) {
    rc = bfpilot_io_open(endpoint, stage, O_WRONLY | O_CREAT | O_EXCL, 0600, &file);
    if (!rc) owned = 1;
  }
  size_t used = 0;
  while (!rc && used < size) {
    if (job_cancelled()) { rc = -ECANCELED; break; }
    ssize_t got = bfpilot_io_write(file, data + used, size - used);
    if (got <= 0) { rc = got < 0 ? (int)got : -EIO; break; }
    used += (size_t)got;
  }
  if (!rc) rc = bfpilot_io_sync(file);
  if (file) { int closed = bfpilot_io_close(file); if (!rc) rc = closed; }
  if (!rc && job_cancelled()) rc = -ECANCELED;
  if (!rc) {
    int got = bfpilot_io_lstat(endpoint, path, &current);
    if (expected && (got == -ENOENT || (!got && !remote_identity(expected, &current, 1)))) rc = -EAGAIN;
    else if (!expected && !got) rc = -EEXIST;
    else if (got && got != -ENOENT) rc = got;
    if (!rc) rc = bfpilot_io_rename(endpoint, stage, path, expected != NULL);
    if (!rc) owned = 0;
  }
  if (owned) {
    int cleaned = bfpilot_io_remove(endpoint, stage, 0);
    if (cleaned) bfpilot_log("SMB text stage retained rc=%d path=%s", cleaned, stage);
    if (!rc && cleaned) rc = cleaned;
  }
  bfpilot_io_end(endpoint);
  if (rc) errno = -rc;
  return rc ? -1 : 0;
}
