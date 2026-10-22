/* Included by transfer.c after copy_arg. Reuses its serialized job owner and
 * telemetry, while keeping the existing local-only fast paths unchanged. */
#define REMOTE_MANIFEST_MAX (32U * 1024U * 1024U)
struct remote_record {
  char *relative;
  struct stat st;
};
struct remote_copy {
  bfpilot_io_t *source, *destination;
  const char *source_path, *stage, *final;
  int moving, stage_owned;
  struct remote_record *records;
  size_t count, capacity, names;
  int space_checked, space_known;
  unsigned long long space_available;
};
static int remote_cancelled(void *context) {
  (void)context;
  return job_cancelled();
}
static int remote_identity(const struct stat *a, const struct stat *b,
                           int contents) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
         (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT) &&
         (!contents ||
          (a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec));
}
static int remote_record_add(struct remote_copy *copy, const char *relative,
                             const struct stat *st) {
  if (!copy->moving)
    return 0;
  size_t bytes = strlen(relative) + 1, capacity = copy->capacity;
  if (copy->count == capacity)
    capacity = capacity ? capacity * 2 : 128;
  if (capacity > REMOTE_MANIFEST_MAX / sizeof(*copy->records) ||
      bytes > REMOTE_MANIFEST_MAX - copy->names ||
      capacity * sizeof(*copy->records) >
          REMOTE_MANIFEST_MAX - copy->names - bytes)
    return -E2BIG;
  if (capacity != copy->capacity) {
    void *records = realloc(copy->records, capacity * sizeof(*copy->records));
    if (!records)
      return -ENOMEM;
    copy->records = records;
    copy->capacity = capacity;
  }
  char *name = strdup(relative);
  if (!name)
    return -ENOMEM;
  copy->records[copy->count++] = (struct remote_record){name, *st};
  copy->names += bytes;
  return 0;
}
static int remote_record_compare(const void *a, const void *b) {
  return strcmp(((const struct remote_record *)a)->relative,
                ((const struct remote_record *)b)->relative);
}
static const struct stat *remote_record_find(struct remote_copy *copy,
                                             const char *relative) {
  size_t lo = 0, hi = copy->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(relative, copy->records[mid].relative);
    if (!order)
      return &copy->records[mid].st;
    if (order < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}
static int remote_copy_file(struct remote_copy *copy, const char *source,
                            const char *destination,
                            const struct stat *expected) {
  bfpilot_io_file_t *in = NULL, *out = NULL;
  unsigned char *buffer = NULL;
  int rc = 0, created = 0;
  if (expected->st_size < 0 ||
      expected->st_size > LONG_MAX - atomic_load(&g_job.total_bytes) ||
      atomic_load(&g_job.total_files) == INT_MAX)
    return -EOVERFLOW;
  atomic_fetch_add(&g_job.total_files, 1);
  atomic_fetch_add(&g_job.total_bytes, (long)expected->st_size);
  if (bfpilot_io_remote(copy->destination)) {
    /* Query once per owned job, then account for newly staged bytes. Avoid a
       network capacity round trip for every small file. Writes still enforce
       concurrent server changes and quotas when statvfs is unavailable. */
    if(!copy->space_checked) {
      bfpilot_io_space_t space;
      copy->space_checked=1;
      if(!bfpilot_io_space(copy->destination,bfpilot_io_root(copy->destination),&space)) {
        copy->space_known=1;copy->space_available=space.available;
      }
    }
    if(copy->space_known) {
      if((unsigned long long)expected->st_size>copy->space_available)return -ENOSPC;
      copy->space_available-=(unsigned long long)expected->st_size;
    }
  } else {
    char parent[1024];
    snprintf(parent, sizeof(parent), "%s", destination);
    char *slash = strrchr(parent, '/');
    if (slash == parent)
      slash[1] = 0;
    else if (slash)
      *slash = 0;
    unsigned long long available, needed;
    int space = check_free_space_for_bytes(parent, expected->st_size,
                                           &available, &needed);
    if (space)
      return space > 0 ? -ENOSPC : -errno;
  }
  rc = bfpilot_io_open(copy->source, source, O_RDONLY, 0, &in);
  struct stat opened;
  if (!rc) {
    rc = bfpilot_io_fstat(in, &opened);
    if (!rc && !remote_identity(expected, &opened, 1))
      rc = -EAGAIN;
  }
  if (!rc) {
    rc = bfpilot_io_open(copy->destination, destination,
                         O_WRONLY | O_CREAT | O_EXCL, 0600, &out);
    if (!rc) {
      created = 1;
      if (!strcmp(destination, copy->stage))
        copy->stage_owned = 1;
    }
  }
  if (!rc) {
    buffer = malloc(1024U * 1024U);
    if (!buffer)
      rc = -ENOMEM;
  }
  unsigned long long done = 0;
  while (!rc && done < (unsigned long long)expected->st_size) {
    if (job_cancelled()) {
      rc = -ECANCELED;
      break;
    }
    size_t want = (unsigned long long)expected->st_size - done > 1024U * 1024U
                      ? 1024U * 1024U
                      : (size_t)((unsigned long long)expected->st_size - done);
    ssize_t got = bfpilot_io_read(in, buffer, want);
    if (got <= 0) {
      rc = got < 0 ? (int)got : -EIO;
      break;
    }
    atomic_fetch_add(&g_job.read_bytes, (long)got);
    size_t offset = 0;
    while (!rc && offset < (size_t)got) {
      if (job_cancelled()) {
        rc = -ECANCELED;
        break;
      }
      ssize_t n = bfpilot_io_write(out, buffer + offset, (size_t)got - offset);
      if (n <= 0) {
        rc = n < 0 ? (int)n : -EIO;
        break;
      }
      offset += (size_t)n;
      done += (unsigned long long)n;
      atomic_fetch_add(&g_job.written_bytes, (long)n);
      atomic_fetch_add(&g_job.copied_bytes, (long)n);
    }
  }
  if (!rc) {
    unsigned char extra;
    ssize_t n = bfpilot_io_read(in, &extra, 1);
    if (n != 0)
      rc = n < 0 ? (int)n : -EAGAIN;
  }
  if (!rc) {
    rc = bfpilot_io_fstat(in, &opened);
    if (!rc && !remote_identity(expected, &opened, 1))
      rc = -EAGAIN;
  }
  if (!rc)
    rc = bfpilot_io_chmod_file(out, 0777);
  if (!rc) {
    rc = bfpilot_io_sync(out);
    if (!rc)
      atomic_fetch_add(&g_job.writeback_syncs, 1);
  }
  if (in) {
    int closed = bfpilot_io_close(in);
    if (!rc)
      rc = closed;
  }
  if (out) {
    int closed = bfpilot_io_close(out);
    if (!rc)
      rc = closed;
  }
  free(buffer);
  if (rc == -EAGAIN)
    job_set_error("source changed during transfer: %s", source);
  if (rc) {
    atomic_fetch_add(&g_job.failed_files, 1);
    if (created)
      (void)bfpilot_io_remove(copy->destination, destination, 0);
  } else {
    atomic_fetch_add(&g_job.done_files, 1);
    atomic_fetch_add(&g_job.serial_files, 1);
  }
  return rc;
}
static int remote_copy_entry(const char *path, const char *relative,
                             const struct stat *st, void *context) {
  struct remote_copy *copy = context;
  char destination[1024];
  int rc;
  if (*relative) {
    if (join_path_checked(destination, sizeof(destination), copy->stage,
                          relative) != 0)
      return -errno;
  } else
    snprintf(destination, sizeof(destination), "%s", copy->stage);
  rc = remote_record_add(copy, relative, st);
  if (rc)
    return rc;
  job_set_current(path);
  if (S_ISDIR(st->st_mode))
    return *relative ? bfpilot_io_mkdir(copy->destination, destination, 0777)
                     : 0;
  return remote_copy_file(copy, path, destination, st);
}
struct remote_remove {
  bfpilot_io_t *endpoint;
  struct remote_copy *copy;
  int progress, verify;
};
static int remote_remove_entry(const char *path, const char *relative,
                               const struct stat *st, void *context) {
  struct remote_remove *remove = context;
  struct stat current;
  int rc = bfpilot_io_lstat(remove->endpoint, path, &current);
  if (rc)
    return rc;
  if (!remote_identity(st, &current, S_ISREG(st->st_mode))) {
    job_set_error("source changed before removal: %s", path);
    return -EAGAIN;
  }
  if (remove->verify) {
    const struct stat *old = remote_record_find(remove->copy, relative);
    if (!old || !remote_identity(old, &current, S_ISREG(old->st_mode))) {
      job_set_error("source changed before removal: %s", path);
      return -EAGAIN;
    }
  }
  if (remove->progress)
    job_set_current(path);
  rc = bfpilot_io_remove(remove->endpoint, path, S_ISDIR(current.st_mode));
  if (!rc && remove->progress)
    atomic_fetch_add(&g_job.done_files, 1);
  return rc;
}
static int remote_remove_tree(bfpilot_io_t *endpoint, const char *path,
                              int progress, struct remote_copy *verify) {
  struct remote_remove remove = {endpoint, verify, progress, verify != NULL};
  int rc =
      bfpilot_io_walk(endpoint, path, 1, remote_remove_entry,
                      progress || verify ? remote_cancelled : NULL, &remove);
  return rc == -ENOENT && !progress && !verify ? 0 : rc;
}
static int remote_verify_source(const char *path, const char *relative,
                                const struct stat *st, void *context) {
  struct remote_copy *copy = context;
  const struct stat *old = remote_record_find(copy, relative);
  if (!old || !remote_identity(old, st, 1)) {
    job_set_error("source changed before move cleanup: %s", path);
    return -EAGAIN;
  }
  return 0;
}
static int remote_merge_entry(const char *path, const char *relative,
                              const struct stat *st, void *context) {
  struct remote_copy *copy = context;
  char target[1024];
  if (*relative) {
    if (join_path_checked(target, sizeof(target), copy->final, relative) != 0)
      return -errno;
  } else
    snprintf(target, sizeof(target), "%s", copy->final);
  struct stat existing;
  int rc = bfpilot_io_lstat(copy->destination, target, &existing);
  if (rc && rc != -ENOENT)
    return rc;
  if (!rc && ((existing.st_mode & S_IFMT) != (st->st_mode & S_IFMT)))
    return -EEXIST;
  if (S_ISDIR(st->st_mode))
    return rc == -ENOENT ? bfpilot_io_mkdir(copy->destination, target, 0777)
                         : 0;
  job_set_current(target);
  return bfpilot_io_rename(copy->destination, path, target, 1);
}
static int remote_validate_merge(const char *path, const char *relative,
                                 const struct stat *st, void *context) {
  (void)path;
  struct remote_copy *copy = context;
  char target[1024];
  if (*relative) {
    if (join_path_checked(target, sizeof(target), copy->final, relative) != 0)
      return -errno;
  } else
    snprintf(target, sizeof(target), "%s", copy->final);
  struct stat current;
  int rc = bfpilot_io_lstat(copy->destination, target, &current);
  if (rc == -ENOENT)
    return 0;
  if (rc)
    return rc;
  return (current.st_mode & S_IFMT) == (st->st_mode & S_IFMT) ? 0 : -EEXIST;
}
static int remote_stage(struct remote_copy *copy, const char *parent,
                        char *path, size_t capacity) {
  if (!bfpilot_io_remote(copy->destination))
    return bfpilot_stage_path_avoiding(
               parent, "network-copy",
               bfpilot_smb_path(copy->source_path) ? NULL : copy->source_path,
               path, capacity) == 0
               ? 0
               : -errno;
  char base[1024];
  if (join_path_checked(base, sizeof(base), bfpilot_io_root(copy->destination),
                        ".bfpilot-staging") != 0)
    return -errno;
  int rc = bfpilot_io_mkdirs(copy->destination, base, 0700);
  if (rc)
    return rc;
  static atomic_ulong sequence;
  char name[96];
  snprintf(name, sizeof(name), "copy-%ld-%ld-%lu", (long)getpid(),
           monotonic_ms(), atomic_fetch_add(&sequence, 1));
  return join_path_checked(path, capacity, base, name) == 0 ? 0 : -errno;
}
static int remote_copy_run(struct copy_arg *arg) {
  struct remote_copy copy = {0};
  copy.source_path = arg->src;
  copy.moving = arg->is_move;
  char final[1024], stage[1024] = {0};
  struct stat source, target, existing;
  int rc = bfpilot_io_endpoint(arg->src, &copy.source);
  if (!rc)
    rc = bfpilot_io_endpoint(arg->dst, &copy.destination);
  if (!rc)
    rc = bfpilot_io_lstat(copy.source, arg->src, &source);
  if (!rc && arg->is_move && !strcmp(arg->src, bfpilot_io_root(copy.source)))
    rc = -EPERM;
  if (!rc && !S_ISREG(source.st_mode) && !S_ISDIR(source.st_mode))
    rc = -ENOTSUP;
  if (!rc)
    rc = bfpilot_io_mkdirs(copy.destination, arg->dst, 0777);
  if (!rc)
    rc = bfpilot_io_lstat(copy.destination, arg->dst, &target);
  if (!rc && !S_ISDIR(target.st_mode))
    rc = -ENOTDIR;
  if (!rc && join_path_checked(final, sizeof(final), arg->dst,
                               path_basename(arg->src)) != 0)
    rc = -errno;
  copy.final = final;
  copy.stage = stage;
  if (!rc &&
      (!strcmp(arg->src, final) ||
       (S_ISDIR(source.st_mode) && path_is_same_or_child(arg->src, final))))
    rc = -EINVAL;
  int exists = 0, merge = 0;
  if (!rc) {
    int got = bfpilot_io_lstat(copy.destination, final, &existing);
    if (!got) {
      exists = 1;
      if (remote_identity(&source, &existing, 0))
        rc = -EINVAL;
      else if (!arg->overwrite)
        rc = -EEXIST;
      else if ((source.st_mode & S_IFMT) != (existing.st_mode & S_IFMT))
        rc = -EEXIST;
      else
        merge = S_ISDIR(source.st_mode);
    } else if (got != -ENOENT)
      rc = got;
  }
  if (!rc && S_ISDIR(source.st_mode)) {
    char ancestor[1024];
    snprintf(ancestor, sizeof(ancestor), "%s", arg->dst);
    for (;;) {
      struct stat current;
      int got = bfpilot_io_lstat(copy.destination, ancestor, &current);
      if (got) {
        rc = got;
        break;
      }
      if (remote_identity(&source, &current, 0)) {
        rc = -EINVAL;
        break;
      }
      if (!strcmp(ancestor, bfpilot_io_root(copy.destination)) ||
          !strcmp(ancestor, "/"))
        break;
      char *slash = strrchr(ancestor, '/');
      if (!slash) {
        rc = -EINVAL;
        break;
      }
      if (slash == ancestor)
        slash[1] = 0;
      else
        *slash = 0;
    }
  }
  if (!rc) {
    pthread_mutex_lock(&g_job.lock);
    snprintf(g_job.source, sizeof(g_job.source), "%s", arg->src);
    snprintf(g_job.destination, sizeof(g_job.destination), "%s", final);
    g_job.source_dev = bfpilot_io_remote(copy.source)?0:source.st_dev;
    g_job.destination_dev = bfpilot_io_remote(copy.destination)?0:target.st_dev;
    pthread_mutex_unlock(&g_job.lock);
    atomic_store(&g_job.total_known, 0);
  }
  int renamed = 0;
  if (!rc && arg->is_move && !merge && bfpilot_io_remote(copy.source) &&
      bfpilot_io_remote(copy.destination) &&
      !strcmp(bfpilot_io_root(copy.source),
              bfpilot_io_root(copy.destination))) {
    if (job_cancelled())
      rc = -ECANCELED;
    else {
      rc = bfpilot_io_rename(copy.source, arg->src, final, arg->overwrite);
      if (!rc) {
        renamed = 1;
        job_set_current("Moved by server rename");
      } else if (rc == -EXDEV || rc == -ENOTSUP)
        rc = 0;
    }
  }
  if (!rc && !renamed)
    rc = remote_stage(&copy, arg->dst, stage, sizeof(stage));
  if (!rc && !renamed && S_ISDIR(source.st_mode)) {
    rc = bfpilot_io_mkdir(copy.destination, stage, 0777);
    if (!rc)
      copy.stage_owned = 1;
  }
  if (!rc && !renamed)
    rc = bfpilot_io_walk(copy.source, arg->src, 0, remote_copy_entry,
                         remote_cancelled, &copy);
  if (!rc && !renamed && job_cancelled())
    rc = -ECANCELED;
  if (!rc && !renamed) {
    job_set_current("Publishing complete output");
    if (merge) {
      rc = bfpilot_io_walk(copy.destination, stage, 0, remote_validate_merge,
                           remote_cancelled, &copy);
      if (!rc)
        rc = bfpilot_io_walk(copy.destination, stage, 0, remote_merge_entry,
                             remote_cancelled, &copy);
    } else {
      struct stat now;
      int got = bfpilot_io_lstat(copy.destination, final, &now);
      if (!exists && got == 0)
        rc = -EEXIST;
      else if (got && got != -ENOENT)
        rc = got;
      else if (exists && (got || !remote_identity(&existing, &now, 1)))
        rc = -EAGAIN;
      else
        rc = bfpilot_io_rename(copy.destination, stage, final, arg->overwrite);
      if (!rc)
        copy.stage_owned = 0;
    }
  }
  if (copy.stage_owned) {
    int cleaned = remote_remove_tree(copy.destination, stage, 0, NULL);
    if (!rc && cleaned)
      rc = cleaned;
    if (cleaned)
      bfpilot_log("network stage retained cleanup_rc=%d stage=%s", cleaned,
                  stage);
  }
  if (!rc && !renamed && arg->is_move) {
    qsort(copy.records, copy.count, sizeof(*copy.records),
          remote_record_compare);
    job_set_current("Verifying moved source before cleanup");
    rc = bfpilot_io_walk(copy.source, arg->src, 0, remote_verify_source,
                         remote_cancelled, &copy);
    if (!rc)
      rc = remote_remove_tree(copy.source, arg->src, 0, &copy);
  }
  for (size_t i = 0; i < copy.count; i++)
    free(copy.records[i].relative);
  free(copy.records);
  bfpilot_io_end(copy.source);
  bfpilot_io_end(copy.destination);
  return rc;
}
static void *remote_copy_worker(void *context) {
  struct copy_arg *arg = context;
  int rc = remote_copy_run(arg);
  if (rc)
    errno = -rc;
  else
    bfpilot_search_mark_stale(arg->is_move ? "network move" : "network copy");
  job_end(rc, rc ? strerror(-rc) : NULL);
  free(arg);
  return NULL;
}
