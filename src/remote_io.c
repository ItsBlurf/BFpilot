/* Bounded endpoint primitives for mixed local/SMB workers. Original local
 * transfer fast paths remain separate. No process-wide libc interposition. */
#include "remote_io.h"
#include "fs_at.h"
#include "path_ops.h"
#include "smb.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// libsmb2 public headers require time_t and SMB protocol types first.
#include <smb2/smb2.h>

#include <smb2/libsmb2.h>

struct bfpilot_io {
  bfpilot_smb_session_t *session;
  struct smb2_context *smb;
  char prefix[1024];
  dev_t device;
};
struct bfpilot_io_file {
  bfpilot_io_t *endpoint;
  int fd;
  struct smb2fh *remote;
};
struct bfpilot_io_dir {
  bfpilot_io_t *endpoint;
  DIR *local;
  struct smb2dir *remote;
  unsigned count;
};
int bfpilot_io_seek(bfpilot_io_file_t *file, off_t offset) {
  if (!file || offset < 0) return -EINVAL;
  if (file->remote) {
    int64_t rc = smb2_lseek(file->endpoint->smb, file->remote, offset,
                            SEEK_SET, NULL);
    return rc < 0 ? (int)rc : 0;
  }
  return lseek(file->fd, offset, SEEK_SET) < 0 ? -errno : 0;
}
static int remote_path(bfpilot_io_t *endpoint, const char *path,
                       const char **relative) {
  if (!endpoint || !path || !endpoint->smb)
    return -EINVAL;
  int rc = bfpilot_smb_validate_path(path);
  if (rc)
    return rc;
  size_t n = strlen(endpoint->prefix);
  if (strncmp(path, endpoint->prefix, n) || (path[n] && path[n] != '/'))
    return -EXDEV;
  *relative = path + n + (path[n] == '/');
  return 0;
}
int bfpilot_io_endpoint(const char *path, bfpilot_io_t **out) {
  *out = NULL;
  if (!bfpilot_path_is_safe(path) || strlen(path) >= 1024)
    return -EINVAL;
  bfpilot_io_t *endpoint = calloc(1, sizeof(*endpoint));
  if (!endpoint)
    return -ENOMEM;
  if (bfpilot_smb_path(path)) {
    int rc = bfpilot_smb_connect(path, &endpoint->session);
    if (rc) {
      free(endpoint);
      return rc;
    }
    endpoint->smb = bfpilot_smb_context(endpoint->session);
    const char *relative = bfpilot_smb_relative(endpoint->session);
    size_t prefix = strlen(path) - strlen(relative);
    while (prefix && path[prefix - 1] == '/')
      prefix--;
    memcpy(endpoint->prefix, path, prefix);
    endpoint->prefix[prefix] = 0;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < prefix; i++)
      hash = (hash ^ (unsigned char)path[i]) * 16777619u;
    endpoint->device = (dev_t)(0x40000000u | (hash & 0x3fffffffu));
  }
  *out = endpoint;
  return 0;
}
void bfpilot_io_end(bfpilot_io_t *endpoint) {
  if (!endpoint)
    return;
  bfpilot_smb_disconnect(endpoint->session);
  free(endpoint);
}
int bfpilot_io_remote(const bfpilot_io_t *endpoint) {
  return endpoint && endpoint->smb != NULL;
}
const char *bfpilot_io_root(const bfpilot_io_t *endpoint) {
  return endpoint && endpoint->smb ? endpoint->prefix : "/";
}
int bfpilot_io_mkdirs(bfpilot_io_t *endpoint,const char *path,mode_t mode) {
  if(!endpoint||!path||strlen(path)>=1024||!bfpilot_path_is_safe(path))return -EINVAL;
  char buffer[1024];snprintf(buffer,sizeof(buffer),"%s",path);
  size_t start=endpoint->smb?strlen(endpoint->prefix):1;
  if(endpoint->smb){const char *relative;int rc=remote_path(endpoint,path,&relative);if(rc)return rc;}
  for(size_t i=start;i<=strlen(path);i++) {
    if(buffer[i]!='/'&&buffer[i])continue;
    char saved=buffer[i];buffer[i]=0;
    if(buffer[0]) {
      struct stat st;int rc=bfpilot_io_lstat(endpoint,buffer,&st);
      if(rc==-ENOENT)rc=bfpilot_io_mkdir(endpoint,buffer,mode);
      else if(!rc&&!S_ISDIR(st.st_mode))rc=-ENOTDIR;
      if(rc){buffer[i]=saved;return rc;}
    }
    buffer[i]=saved;
  }
  return 0;
}
static int convert_stat(bfpilot_io_t *endpoint,
                        const struct smb2_stat_64 *source, struct stat *out) {
  if (source->smb2_size > LLONG_MAX || source->smb2_mtime > LLONG_MAX)
    return -EOVERFLOW;
  memset(out, 0, sizeof(*out));
  out->st_dev = endpoint->device;
  out->st_ino = source->smb2_ino;
  out->st_size = (off_t)source->smb2_size;
  out->st_nlink = source->smb2_nlink;
  if (source->smb2_type == SMB2_TYPE_DIRECTORY)
    out->st_mode = S_IFDIR | 0777;
  else if (source->smb2_type == SMB2_TYPE_FILE)
    out->st_mode = S_IFREG | 0666;
  else if (source->smb2_type == SMB2_TYPE_LINK)
    out->st_mode = S_IFLNK;
  else
    return -ENOTSUP;
  out->st_mtim.tv_sec = source->smb2_mtime;
  out->st_mtim.tv_nsec = source->smb2_mtime_nsec;
  return 0;
}
int bfpilot_io_lstat(bfpilot_io_t *endpoint, const char *path,
                     struct stat *out) {
  if (!endpoint || !path || !out)
    return -EINVAL;
  if (!endpoint->smb)
    return lstat(path, out) == 0 ? 0 : -errno;
  const char *relative;
  int rc = remote_path(endpoint, path, &relative);
  if (rc)
    return rc;
  struct smb2_stat_64 st;
  rc = smb2_lstat(endpoint->smb, relative, &st);
  return rc ? rc : convert_stat(endpoint, &st, out);
}
int bfpilot_io_open(bfpilot_io_t *endpoint, const char *path, int flags,
                    mode_t mode, bfpilot_io_file_t **out) {
  *out = NULL;
  if (!endpoint)
    return -EINVAL;
  bfpilot_io_file_t *file = calloc(1, sizeof(*file));
  if (!file)
    return -ENOMEM;
  file->endpoint = endpoint;
  file->fd = -1;
  int rc = 0;
  if (endpoint->smb) {
    const char *relative;
    rc = remote_path(endpoint, path, &relative);
    if (!rc) {
      file->remote = smb2_open(endpoint->smb, relative, flags | O_NOFOLLOW);
      if (!file->remote) {
        int status = smb2_get_nterror(endpoint->smb);
        rc = status ? -nterror_to_errno(status) : -EIO;
      }
    }
  } else {
    file->fd = open(path, flags | O_NOFOLLOW, mode);
    if (file->fd < 0)
      rc = -errno;
  }
  if (!rc) {
    struct stat st;
    rc = bfpilot_io_fstat(file, &st);
    if (!rc && !S_ISREG(st.st_mode))
      rc = S_ISLNK(st.st_mode) ? -ELOOP : -ENOTSUP;
  }
  if (rc) {
    if (file->remote)
      smb2_close(endpoint->smb, file->remote);
    if (file->fd >= 0)
      close(file->fd);
    free(file);
    return rc;
  }
  *out = file;
  return 0;
}
ssize_t bfpilot_io_read(bfpilot_io_file_t *file, void *data, size_t size) {
  if (!file)
    return -EINVAL;
  if (file->remote) {
    uint32_t maximum = smb2_get_max_read_size(file->endpoint->smb);
    if (!maximum)
      return -EIO;
    uint32_t count = size > maximum ? maximum : (uint32_t)size;
    return smb2_read(file->endpoint->smb, file->remote, data, count);
  }
  ssize_t n;
  do {
    n = read(file->fd, data, size);
  } while (n < 0 && errno == EINTR);
  return n < 0 ? -errno : n;
}
ssize_t bfpilot_io_write(bfpilot_io_file_t *file, const void *data,
                         size_t size) {
  if (!file)
    return -EINVAL;
  if (file->remote) {
    uint32_t maximum = smb2_get_max_write_size(file->endpoint->smb);
    if (!maximum)
      return -EIO;
    uint32_t count = size > maximum ? maximum : (uint32_t)size;
    return smb2_write(file->endpoint->smb, file->remote, data, count);
  }
  ssize_t n;
  do {
    n = write(file->fd, data, size);
  } while (n < 0 && errno == EINTR);
  return n < 0 ? -errno : n;
}
int bfpilot_io_fstat(bfpilot_io_file_t *file, struct stat *out) {
  if (!file || !out)
    return -EINVAL;
  if (file->remote) {
    struct smb2_stat_64 st;
    int rc = smb2_fstat(file->endpoint->smb, file->remote, &st);
    return rc ? rc : convert_stat(file->endpoint, &st, out);
  }
  return fstat(file->fd, out) == 0 ? 0 : -errno;
}
int bfpilot_io_sync(bfpilot_io_file_t *file) {
  if (!file)
    return -EINVAL;
  return file->remote           ? smb2_fsync(file->endpoint->smb, file->remote)
         : fsync(file->fd) == 0 ? 0
                                : -errno;
}
int bfpilot_io_chmod_file(bfpilot_io_file_t *file,mode_t mode) {
  if(!file)return -EINVAL;
  // SMB permissions are server ACLs, not synthetic Unix mode bits.
  if(file->remote)return 0;
  return fchmod(file->fd,mode)==0?0:-errno;
}
int bfpilot_io_close(bfpilot_io_file_t *file) {
  if (!file)
    return -EINVAL;
  int rc = file->remote ? smb2_close(file->endpoint->smb, file->remote)
           : close(file->fd) == 0 ? 0
                                  : -errno;
  free(file);
  return rc;
}
int bfpilot_io_mkdir(bfpilot_io_t *endpoint, const char *path, mode_t mode) {
  if (!endpoint)
    return -EINVAL;
  if (!endpoint->smb)
    return mkdir(path, mode) == 0 ? 0 : -errno;
  const char *relative;
  int rc = remote_path(endpoint, path, &relative);
  return rc ? rc : smb2_mkdir(endpoint->smb, relative);
}
int bfpilot_io_remove(bfpilot_io_t *endpoint, const char *path, int directory) {
  if (!endpoint)
    return -EINVAL;
  if (!endpoint->smb)
    return (directory ? rmdir(path) : unlink(path)) == 0 ? 0 : -errno;
  const char *relative;
  int rc = remote_path(endpoint, path, &relative);
  if (rc)
    return rc;
  if (!relative[0])
    return -EPERM;
  return directory ? smb2_rmdir(endpoint->smb, relative)
                   : smb2_unlink(endpoint->smb, relative);
}
int bfpilot_io_rename(bfpilot_io_t *endpoint, const char *source,
                      const char *destination, int replace) {
  if (!endpoint)
    return -EINVAL;
  if (!endpoint->smb) {
    if (!replace) {
      struct stat st;
      if (lstat(destination, &st) == 0)
        return -EEXIST;
      if (errno != ENOENT)
        return -errno;
    }
    return rename(source, destination) == 0 ? 0 : -errno;
  }
  const char *oldpath, *newpath;
  int rc = remote_path(endpoint, source, &oldpath);
  if (!rc)
    rc = remote_path(endpoint, destination, &newpath);
  if (rc)
    return rc;
  if (!*oldpath || !*newpath)
    return -EPERM;
  return replace ? smb2_rename_replace(endpoint->smb, oldpath, newpath)
                 : smb2_rename(endpoint->smb, oldpath, newpath);
}
int bfpilot_io_opendir(bfpilot_io_t *endpoint, const char *path,
                       bfpilot_io_dir_t **out) {
  *out = NULL;
  if (!endpoint)
    return -EINVAL;
  struct stat before;
  int rc = bfpilot_io_lstat(endpoint, path, &before);
  if (rc)
    return rc;
  if (!S_ISDIR(before.st_mode))
    return -ENOTDIR;
  bfpilot_io_dir_t *dir = calloc(1, sizeof(*dir));
  if (!dir)
    return -ENOMEM;
  dir->endpoint = endpoint;
  if (endpoint->smb) {
    const char *relative;
    rc = remote_path(endpoint, path, &relative);
    if (!rc) {
      dir->remote = smb2_opendir(endpoint->smb, relative);
      if (!dir->remote) {
        int status = smb2_get_nterror(endpoint->smb);
        rc = status ? -nterror_to_errno(status) : -EIO;
      }
    }
  } else {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0)
      rc = -errno;
    if (!rc) {
      struct stat actual;
      if (fstat(fd, &actual) != 0)
        rc = -errno;
      else if (actual.st_dev != before.st_dev || actual.st_ino != before.st_ino)
        rc = -EAGAIN;
    }
    if (!rc) {
      dir->local = fdopendir(fd);
      if (!dir->local)
        rc = -errno;
    }
    if (rc && fd >= 0)
      close(fd);
  }
  if (rc) {
    free(dir);
    return rc;
  }
  *out = dir;
  return 0;
}
int bfpilot_io_readdir(bfpilot_io_dir_t *dir, char *name, size_t capacity,
                       struct stat *st) {
  if (!dir || !name || !capacity || !st)
    return -EINVAL;
  for (;;) {
    const char *value;
    struct smb2dirent *remote = NULL;
    struct dirent *local = NULL;
    if (dir->remote) {
      remote = smb2_readdir(dir->endpoint->smb, dir->remote);
      if (!remote)
        return 0;
      value = remote->name;
    } else {
      errno = 0;
      local = readdir(dir->local);
      if (!local)
        return errno ? -errno : 0;
      value = local->d_name;
    }
    if (!strcmp(value, ".") || !strcmp(value, ".."))
      continue;
    size_t length = strlen(value);
    if (!length || length >= capacity || strchr(value, '/') ||
        (dir->remote && strchr(value, '\\')))
      return -ENAMETOOLONG;
    if (++dir->count > 100000)
      return -E2BIG;
    int rc = remote ? convert_stat(dir->endpoint, &remote->st, st)
             : bfpilot_fs_fstatat(dirfd(dir->local), value, st,
                                  AT_SYMLINK_NOFOLLOW) == 0
                 ? 0
                 : -errno;
    if (rc)
      return rc;
    memcpy(name, value, length + 1);
    return 1;
  }
}
int bfpilot_io_closedir(bfpilot_io_dir_t *dir) {
  if (!dir)
    return -EINVAL;
  int rc = 0;
  if (dir->remote)
    smb2_closedir(dir->endpoint->smb, dir->remote);
  else if (closedir(dir->local) != 0)
    rc = -errno;
  free(dir);
  return rc;
}
