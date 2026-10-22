#include "pkg_source_wire.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BFPS_TEST_POSIX
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
static void stamp_init(bfpilot_pkg_source_stamp_t *s) {
  memset(s, 0, sizeof(*s));
  s->size = 8192;
  s->inode = 1234567;
  s->mtime_sec = 1700000000;
  s->mtime_nsec = 123456789;
  memcpy(s->header, "\177CNT", 4);
  const char *id = "EP0000-PPSA99991_00-OWNEDPKGTEST0000";
  memcpy(s->header + 0x40, id, strlen(id));
  bfps_put64(s->header + 0x430, s->size);
}
#ifdef BFPS_TEST_POSIX
typedef struct {
  int fd;
  const unsigned char *data;
  size_t size;
} fragment_t;
static void *fragment(void *context) {
  fragment_t *f = context;
  for (size_t i = 0; i < f->size; i++) {
    assert(send(f->fd, f->data + i, 1, 0) == 1);
    usleep(100);
  }
  close(f->fd);
  return NULL;
}
static void framing_tests(void) {
  unsigned char header[16] = {
      'B', 'F', 'S', '1', 0, BFPS_REGISTER, 0, 0, 0, 0, 0, 4, 1, 2, 3, 4};
  int pair[2];
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  fragment_t f = {pair[1], header, sizeof(header)};
  pthread_t t;
  assert(!pthread_create(&t, NULL, fragment, &f));
  unsigned type = 0;
  size_t size = 0;
  unsigned char data[16];
  assert(!bfps_receive_frame(pair[0], &type, data, sizeof(data), &size) &&
         type == BFPS_REGISTER && size == 4 && !memcmp(data, header + 12, 4));
  close(pair[0]);
  pthread_join(t, NULL);
  for (unsigned truncated = 0; truncated < 16; truncated++) {
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    if (truncated)
      assert(send(pair[1], header, truncated, 0) == (ssize_t)truncated);
    close(pair[1]);
    assert(bfps_receive_frame(pair[0], &type, data, sizeof(data), &size) < 0);
    close(pair[0]);
  }
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  header[6] = 1;
  assert(send(pair[1], header, 12, 0) == 12);
  assert(bfps_receive_frame(pair[0], &type, data, sizeof(data), &size) ==
         -EPROTO);
  close(pair[0]);
  close(pair[1]);
  header[6] = 0;
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  bfps_put32(header + 8, BFPILOT_PKG_SOURCE_FRAME_MAX + 1);
  assert(send(pair[1], header, 12, 0) == 12);
  assert(bfps_receive_frame(pair[0], &type, data, sizeof(data), &size) ==
         -EMSGSIZE);
  close(pair[0]);
  close(pair[1]);
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  assert(!bfps_send_frame(pair[1], BFPS_REGISTERED, "token", 5));
  assert(!bfps_receive_frame(pair[0], &type, data, sizeof(data), &size) &&
         type == BFPS_REGISTERED && size == 5 && !memcmp(data, "token", 5));
  close(pair[0]);
  close(pair[1]);
}
#endif
int main(void) {
  bfpilot_pkg_source_stamp_t stamp, decoded;
  stamp_init(&stamp);
  unsigned char buffer[4096], journal[4096];
  size_t size, record_size;
  const char *path = "/smb/85328221df355b81/共享测试/owned.pkg";
  char restored[1024], token[33];
  uint64_t seen;
  assert(!bfps_register_encode(path, &stamp, buffer, sizeof(buffer), &size));
  assert(!bfps_register_decode(buffer, size, restored, sizeof(restored),
                               &decoded) &&
         !strcmp(restored, path) && decoded.inode == stamp.inode &&
         decoded.size == stamp.size &&
         !memcmp(decoded.header, stamp.header, sizeof(stamp.header)));
  for (size_t i = 0; i < size; i++)
    assert(bfps_register_decode(buffer, i, restored, sizeof(restored),
                                &decoded) < 0);
  assert(bfps_register_decode(buffer, size, restored, 8, &decoded) < 0);
  unsigned char save = buffer[size - 1];
  buffer[size - 1] = 0;
  assert(bfps_register_decode(buffer, size, restored, sizeof(restored),
                              &decoded) < 0);
  buffer[size - 1] = save;
  const char *bad[] = {"/data/owned.pkg",
                       "/smb/85328221df355b81/share/../owned.pkg",
                       "/smb/85328221df355b81//owned.pkg",
                       "/smb/85328221df355b81/share/./owned.pkg",
                       "/smb/85328221df355b81/share/owned.pkg\n",
                       "/smb/85328221df355b81/share/owned.txt"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    assert(bfps_register_encode(bad[i], &stamp, buffer, sizeof(buffer), &size) <
           0);
  char edge[1024];
  memset(edge, 'a', sizeof(edge));
  memcpy(edge, "/smb/85328221df355b81/share/", 27);
  edge[1022] = (char)0xf4;
  edge[1023] = 0;
  assert(bfps_register_encode(edge, &stamp, buffer, sizeof(buffer), &size) < 0);
  stamp.mtime_nsec = 1000000000;
  assert(bfps_register_encode(path, &stamp, buffer, sizeof(buffer), &size) < 0);
  stamp_init(&stamp);
  memcpy(stamp.header + 0x47, "NPXS", 4);
  assert(bfps_register_encode(path, &stamp, buffer, sizeof(buffer), &size) < 0);
  stamp_init(&stamp);
  const char *id = "0123456789abcdef0123456789abcdef";
  assert(!bfps_journal_encode(id, 123456, path, &stamp, journal,
                              sizeof(journal), &record_size));
  assert(!bfps_journal_decode(journal, record_size, token, &seen, restored,
                              sizeof(restored), &decoded) &&
         !strcmp(token, id) && seen == 123456 && !strcmp(restored, path));
  for (size_t i = 0; i < record_size; i++) {
    assert(bfps_journal_decode(journal, i, token, &seen, restored,
                               sizeof(restored), &decoded) < 0);
    journal[i] ^= 1;
    assert(bfps_journal_decode(journal, record_size, token, &seen, restored,
                               sizeof(restored), &decoded) < 0);
    journal[i] ^= 1;
  }
  bfpilot_pkg_source_range_t r;
  assert(!bfps_range_parse(NULL, 2048, &r) && !r.partial && r.first == 0 &&
         r.count == 2048);
  assert(!bfps_range_parse("bytes=0-0", 2048, &r) && r.partial &&
         r.first == 0 && r.count == 1);
  assert(!bfps_range_parse("bytes=10-", 2048, &r) && r.first == 10 &&
         r.count == 2038);
  assert(!bfps_range_parse("bytes=-8", 2048, &r) && r.first == 2040 &&
         r.count == 8);
  assert(!bfps_range_parse("bytes=-9999", 2048, &r) && r.first == 0 &&
         r.count == 2048);
  assert(!bfps_range_parse("bytes=0-18446744073709551615", 2048, &r) &&
         r.first == 0 && r.count == 2048);
  const char *ranges[] = {"bytes=1-0",
                          "bytes=2048-",
                          "bytes=-0",
                          "bytes=0-1,2-3",
                          "bytes=18446744073709551616-",
                          "bytes=0-18446744073709551616",
                          "bytes=+1-2",
                          "other=0-1",
                          "bytes= 1-2",
                          "bytes=1-2 ",
                          "bytes=--1"};
  for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
    assert(bfps_range_parse(ranges[i], 2048, &r) < 0);
  assert(!bfps_range_parse(NULL, 0, &r) && !r.count &&
         bfps_range_parse("bytes=0-0", 0, &r) < 0);
  const char *uri =
      BFPILOT_PKG_SOURCE_PREFIX "0123456789abcdef0123456789abcdef.pkg";
  assert(bfps_uri_valid(uri));
  for (size_t i = 0; i < strlen(uri); i++) {
    char *short_uri = malloc(i + 1);
    memcpy(short_uri, uri, i);
    short_uri[i] = 0;
    assert(!bfps_uri_valid(short_uri));
    free(short_uri);
  }
  assert(!bfps_uri_valid(
      "http://localhost:9042/source/0123456789abcdef0123456789abcdef.pkg"));
  assert(!bfps_uri_valid(BFPILOT_PKG_SOURCE_PREFIX
                         "0123456789abcdef0123456789abcdef.pkg/extra"));
#ifdef BFPS_TEST_POSIX
  framing_tests();
#endif
  puts("PKG source registration, range/overflow, every-byte journal "
       "corruption/truncation and strict URI controls pass");
  return 0;
}
