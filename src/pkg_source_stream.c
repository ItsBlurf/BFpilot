#include "pkg_source_stream.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
int bfps_stream_unchanged(bfps_stream_t *stream) {
  struct stat st;
  int rc = bfpilot_io_fstat(stream->file, &st);
  if (!rc && (!S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size != stream->stamp.size ||
      (uint64_t)st.st_ino != stream->stamp.inode ||
      st.st_mtim.tv_sec != stream->stamp.mtime_sec ||
      st.st_mtim.tv_nsec != stream->stamp.mtime_nsec)) rc = -ESTALE;
  return rc;
}
int bfps_stream_close(bfps_stream_t *stream, int rc) {
  if (stream->file) {
    int closed = bfpilot_io_close(stream->file);
    if (!rc) rc = closed;
  }
  bfpilot_io_end(stream->endpoint);
  memset(stream, 0, sizeof(*stream));
  return rc;
}
int bfps_stream_open(const bfps_source_handle_t *handle, bfps_stream_t *stream) {
  memset(stream, 0, sizeof(*stream));
  stream->stamp = handle->stamp;
  int rc = bfpilot_io_endpoint(handle->path, &stream->endpoint);
  if (!rc) rc = bfpilot_io_open(stream->endpoint, handle->path, O_RDONLY, 0, &stream->file);
  if (!rc) rc = bfps_stream_unchanged(stream);
  unsigned char header[BFPILOT_PKG_SOURCE_HEADER_SIZE];
  size_t done = 0;
  while (!rc && done < sizeof(header)) {
    ssize_t got = bfpilot_io_read(stream->file, header + done, sizeof(header) - done);
    if (got <= 0) rc = got < 0 ? (int)got : -EIO;
    else done += (size_t)got;
  }
  if (!rc && memcmp(header, handle->stamp.header, sizeof(header))) rc = -ESTALE;
  if (!rc) rc = bfps_stream_unchanged(stream);
  if (rc) bfps_stream_close(stream, rc);
  return rc;
}
