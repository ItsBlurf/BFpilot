/* Minimal bounded HTTP/1 source endpoint. Range policy follows RFC 9110 14.2:
 * HEAD, unknown units, multi-range and unvalidated If-Range use the full source.
 * https://www.rfc-editor.org/rfc/rfc9110.html#section-14.2 */
#include "pkg_source_http.h"
#include "pkg_source_stream.h"
#include "diag.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#define HTTP_REQUEST_MAX 8192U
#define STREAM_BUFFER (1024U * 1024U)
static int send_bytes(int fd, const void *data, size_t size) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = send(fd, (const char *)data + done, size - done, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 ? -errno : -EIO;
    done += (size_t)n;
  }
  return 0;
}
static void error_reply(int fd, int code, const char *reason, uint64_t size) {
  char response[320], range[96] = "";
  if (code == 416) snprintf(range, sizeof(range), "Content-Range: bytes */%" PRIu64 "\r\n", size);
  int n = snprintf(response, sizeof(response), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\n%sCache-Control: no-store\r\nConnection: close\r\n\r\n", code, reason, range);
  if (n > 0 && (size_t)n < sizeof(response)) (void)send_bytes(fd, response, (size_t)n);
}
static int read_request(int fd, char buffer[HTTP_REQUEST_MAX + 1]) {
  size_t used = 0;
  while (used < HTTP_REQUEST_MAX) {
    ssize_t n = recv(fd, buffer + used, HTTP_REQUEST_MAX - used, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n < 0 ? -errno : -EIO;
    for (ssize_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)buffer[used + (size_t)i];
      if (!c || (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127) return -EINVAL;
    }
    used += (size_t)n; buffer[used] = 0;
    char *end = strstr(buffer, "\r\n\r\n");
    if (end) {
      /* No pipelining/request body is accepted by this read-only endpoint. */
      if ((size_t)(end - buffer) + 4 != used) return -EINVAL;
      return 0;
    }
  }
  return -E2BIG;
}
void bfps_http_serve(int fd) {
  char request[HTTP_REQUEST_MAX + 1];
  int rc = read_request(fd, request);
  if (rc) { error_reply(fd, rc == -E2BIG ? 431 : 400, "Bad Request", 0); return; }
  char *line_end = strstr(request, "\r\n");
  if (!line_end) { error_reply(fd, 400, "Bad Request", 0); return; }
  *line_end = 0;
  char *space = strchr(request, ' ');
  if (!space) { error_reply(fd, 400, "Bad Request", 0); return; }
  *space++ = 0;
  char *version = strchr(space, ' ');
  if (!version) { error_reply(fd, 400, "Bad Request", 0); return; }
  *version++ = 0;
  if (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")) { error_reply(fd, 400, "Bad Request", 0); return; }
  int head = !strcmp(request, "HEAD");
  if (!head && strcmp(request, "GET")) { error_reply(fd, 405, "Method Not Allowed", 0); return; }
  if (strlen(space) != 8 + 32 + 4 || strncmp(space, "/source/", 8) || strcmp(space + 40, ".pkg") || !bfps_token_valid(space + 8, 32)) { error_reply(fd, 404, "Not Found", 0); return; }
  char token[33]; memcpy(token, space + 8, 32); token[32] = 0;
  char *range = NULL;
  int if_range = 0, seen_host = 0;
  for (char *line = line_end + 2; *line;) {
    char *end = strstr(line, "\r\n");
    if (!end) { error_reply(fd, 400, "Bad Request", 0); return; }
    *end = 0;
    if (!*line) break;
    char *colon = strchr(line, ':');
    if (!colon || colon == line || *line == ' ' || *line == '\t') { error_reply(fd, 400, "Bad Request", 0); return; }
    *colon++ = 0;
    for (char *c = line; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-')) { error_reply(fd, 400, "Bad Request", 0); return; }
    while (*colon == ' ' || *colon == '\t') colon++;
    char *tail = colon + strlen(colon);
    while (tail > colon && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
    if (!strcasecmp(line, "Range")) {
      if (range) { error_reply(fd, 400, "Bad Request", 0); return; }
      range = colon;
    } else if (!strcasecmp(line, "If-Range")) if_range = 1;
    else if (!strcasecmp(line, "Host")) {
      if (++seen_host != 1 || (strcmp(colon, "127.0.0.1:9042") && strcmp(colon, "127.0.0.1"))) { error_reply(fd, 400, "Bad Request", 0); return; }
    } else if (!strcasecmp(line, "Transfer-Encoding") || (!strcasecmp(line, "Content-Length") && strcmp(colon, "0"))) { error_reply(fd, 400, "Bad Request", 0); return; }
    line = end + 2;
  }
  if (!strcmp(version, "HTTP/1.1") && !seen_host) { error_reply(fd, 400, "Bad Request", 0); return; }
  bfps_source_handle_t handle;
  rc = bfps_store_acquire(token, &handle);
  if (rc) { error_reply(fd, 404, "Not Found", 0); return; }
  bfpilot_pkg_source_range_t selected;
  if (head || if_range || (range && (strncmp(range, "bytes=", 6) || strchr(range, ',')))) range = NULL;
  rc = bfps_range_parse(range, handle.stamp.size, &selected);
  if (rc) { error_reply(fd, 416, "Range Not Satisfiable", handle.stamp.size); bfps_store_release(&handle); return; }
  bfps_stream_t stream;
  rc = bfps_stream_open(&handle, &stream);
  if (rc) { error_reply(fd, rc == -ESTALE ? 409 : 503, "Source Unavailable", 0); bfps_store_release(&handle); return; }
  unsigned char *buffer = head ? NULL : malloc(STREAM_BUFFER);
  if (!head && !buffer) rc = -ENOMEM;
  if (!rc && !head) rc = bfpilot_io_seek(stream.file, (off_t)selected.first);
  if (!rc && head) rc = bfps_stream_close(&stream, 0);
  if (rc) {
    error_reply(fd, 503, "Source Unavailable", 0);
  } else {
    char response[512], content_range[128] = "";
    if (selected.partial) snprintf(content_range, sizeof(content_range), "Content-Range: bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64 "\r\n", selected.first, selected.first + selected.count - 1, handle.stamp.size);
    int length = snprintf(response, sizeof(response), "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %" PRIu64 "\r\n%sAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", selected.partial ? 206 : 200, selected.partial ? "Partial Content" : "OK", selected.count, content_range);
    if (length < 0 || (size_t)length >= sizeof(response)) rc = -EOVERFLOW;
    else rc = send_bytes(fd, response, (size_t)length);
    uint64_t remaining = head ? 0 : selected.count, since_check = 0;
    while (!rc && remaining) {
      size_t amount = remaining > STREAM_BUFFER ? STREAM_BUFFER : (size_t)remaining;
      ssize_t got = bfpilot_io_read(stream.file, buffer, amount);
      if (got <= 0 || (size_t)got > amount) { rc = got < 0 ? (int)got : -EIO; break; }
      remaining -= (uint64_t)got;
      since_check += (uint64_t)got;
      /* Verify/close before sending the final piece, so a final error is a
       * detectably short HTTP body, never an apparently complete response. */
      if (!remaining || since_check >= 64U * 1024U * 1024U) {
        rc = bfps_stream_unchanged(&stream); since_check = 0;
        bfps_store_touch(&handle);
      }
      if (!remaining) rc = bfps_stream_close(&stream, rc);
      if (!rc) rc = send_bytes(fd, buffer, (size_t)got);
    }
  }
  rc = bfps_stream_close(&stream, rc);
  if (!rc) bfps_store_touch(&handle);
  if (rc) bfpilot_log("pkg source: HTTP source request failed rc=%d", rc);
  free(buffer);
  bfps_store_release(&handle);
}
