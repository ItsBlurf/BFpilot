/* Bounded request-owned SMB readers for the shared portable PKG parser. */
#include "pkg_remote.h"
#include "remote_io.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {bfpilot_io_t *endpoint;bfpilot_io_file_t *file;struct stat before;} remote_pkg_t;
static int read_at(void *context,void *buffer,size_t size,uint64_t offset) {
  remote_pkg_t *source=context;
  if(offset>LLONG_MAX)return -EOVERFLOW;
  int rc=bfpilot_io_seek(source->file,(off_t)offset);size_t done=0;
  while(!rc && done<size) {
    ssize_t got=bfpilot_io_read(source->file,(unsigned char *)buffer+done,size-done);
    if(got<=0){rc=got<0?(int)got:-EIO;break;}
    done+=(size_t)got;
  }
  return rc;
}
static int open_reader(const char *path,remote_pkg_t *source,bfpilot_pkg_reader_t *reader) {
  memset(source,0,sizeof(*source));memset(reader,0,sizeof(*reader));
  int rc=bfpilot_io_endpoint(path,&source->endpoint);
  if(!rc)rc=bfpilot_io_open(source->endpoint,path,O_RDONLY,0,&source->file);
  if(!rc)rc=bfpilot_io_fstat(source->file,&source->before);
  if(!rc && source->before.st_size<0)rc=-EINVAL;
  if(!rc){reader->context=source;reader->size=(uint64_t)source->before.st_size;reader->read_at=read_at;}
  return rc;
}
static int close_reader(remote_pkg_t *source,int rc) {
  if(source->file) {
    if(!rc) {
      struct stat after;rc=bfpilot_io_fstat(source->file,&after);
      if(!rc && (after.st_dev!=source->before.st_dev || after.st_ino!=source->before.st_ino ||
         after.st_size!=source->before.st_size || after.st_mtim.tv_sec!=source->before.st_mtim.tv_sec ||
         after.st_mtim.tv_nsec!=source->before.st_mtim.tv_nsec))rc=-EAGAIN;
    }
    int closed=bfpilot_io_close(source->file);if(!rc)rc=closed;
  }
  bfpilot_io_end(source->endpoint);return rc;
}
int bfpilot_pkg_remote_validate(const char *path,bfpilot_pkg_file_info_t *info,
                                 char *error,size_t error_size) {
  if(!info)return -EINVAL;
  if(error && error_size)error[0]=0;
  remote_pkg_t source;bfpilot_pkg_reader_t reader;int rc=open_reader(path,&source,&reader);
  if(!rc)rc=bfpilot_pkg_validate_reader(&reader,info);
  rc=close_reader(&source,rc);
  if(rc && error && error_size)snprintf(error,error_size,"cannot validate SMB PKG: %s",strerror(-rc));
  return rc;
}
int bfpilot_pkg_remote_details(const char *path,bfpilot_pkg_details_t *details) {
  if(!details)return -EINVAL;
  remote_pkg_t source;bfpilot_pkg_reader_t reader;int rc=open_reader(path,&source,&reader);
  if(!rc)rc=bfpilot_pkg_parse_reader(&reader,details);
  return close_reader(&source,rc);
}
int bfpilot_pkg_remote_icon(const char *path,void **buffer,size_t *size) {
  if(!buffer || !size)return -EINVAL;
  *buffer=NULL;*size=0;
  remote_pkg_t source;bfpilot_pkg_reader_t reader;int rc=open_reader(path,&source,&reader);
  if(!rc)rc=bfpilot_pkg_icon_reader(&reader,buffer,size);
  rc=close_reader(&source,rc);
  if(rc){free(*buffer);*buffer=NULL;*size=0;}
  return rc;
}
int bfpilot_pkg_remote_stamp(const char *path, bfpilot_pkg_source_stamp_t *stamp,
                             bfpilot_pkg_file_info_t *info) {
  if (!stamp || !info) return -EINVAL;
  memset(stamp, 0, sizeof(*stamp));
  remote_pkg_t source; bfpilot_pkg_reader_t reader;
  int rc = open_reader(path, &source, &reader);
  if (!rc && (!S_ISREG(source.before.st_mode) || reader.size < sizeof(stamp->header))) rc = -EINVAL;
  if (!rc) rc = read_at(&source, stamp->header, sizeof(stamp->header), 0);
  if (!rc) rc = bfpilot_pkg_header_validate(stamp->header, sizeof(stamp->header), reader.size, info);
  if (!rc && bfpilot_pkg_file_is_system(info)) rc = -ENOTSUP;
  if (!rc) {
    stamp->size = reader.size; stamp->inode = (uint64_t)source.before.st_ino;
    stamp->mtime_sec = source.before.st_mtim.tv_sec;
    stamp->mtime_nsec = source.before.st_mtim.tv_nsec;
  }
  return close_reader(&source, rc);
}
