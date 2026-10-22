#include "../src/pkg_protocol.h"
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#ifdef BFPILOT_NATIVE_TEST
#include <ps5/payload.h>
#endif
struct writer { int fd; const char *data; size_t size, split; };
static void *write_fragments(void *ctx) {
  struct writer *w = ctx;
  send(w->fd, w->data, w->split, 0);
  usleep(1000);
  send(w->fd, w->data + w->split, w->size - w->split, 0);
  shutdown(w->fd, SHUT_WR);
  return NULL;
}
static int trial(const char *data, size_t size, size_t split, int expected, const char *text) {
  int pair[2]; if(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 0;
  struct writer writer = {pair[0], data, size, split};
  pthread_t thread; if(pthread_create(&thread, NULL, write_fragments, &writer) != 0) return 0;
  char buffer[128];
  int rc = bfpilot_pkg_read_request(pair[1], buffer, sizeof(buffer));
  shutdown(pair[1], SHUT_RDWR);
  pthread_join(thread, NULL); close(pair[0]); close(pair[1]);
  return rc == expected && (rc != 0 || !strcmp(buffer, text));
}
int main(void) {
  signal(SIGPIPE, SIG_IGN);
#ifdef BFPILOT_NATIVE_TEST
  FILE *out = fopen("/data/test/bfpilot-pkg-protocol-r3.txt", "wx");
#else
  FILE *out = stdout;
#endif
  if(!out) return 2;
  const char *request = "INSTALL /data/test/Café game/update.pkg\n";
  char expected[128]; snprintf(expected, sizeof(expected), "%s", request);
  expected[strlen(expected)-1] = 0;
  int failures = 0, tests = 0;
  for(size_t split = 1; split < strlen(request); split++) {
    int pass = trial(request, strlen(request), split, 0, expected);
    tests++; failures += !pass;
    if(!pass) fprintf(out, "FAIL fragmented command at %lu\n", (unsigned long)split);
  }
  const char *data[] = {"PING\n", "PING\r\n", "INSTALL /data/a", "BAD\0PING\n"};
  const size_t sizes[] = {5,6,15,9};
  const int errors[] = {0,0,-EPROTO,-EPROTO};
  for(int i=0;i<4;i++) {
    int pass = trial(data[i], sizes[i], 2, errors[i], "PING");
    tests++; failures += !pass;
    fprintf(out, "%s protocol boundary %d\n", pass ? "PASS" : "FAIL", i);
  }
  char large[200]; memset(large, 'x', sizeof(large));
  int pass = trial(large, sizeof(large), 100, -EMSGSIZE, NULL);
  tests++; failures += !pass;
  fprintf(out, "%s oversized command rejected\nSUMMARY tests=%d failures=%d\n", pass ? "PASS" : "FAIL", tests, failures);
#ifdef BFPILOT_NATIVE_TEST
  fclose(out); payload_exit(failures != 0);
#endif
  return failures != 0;
}
