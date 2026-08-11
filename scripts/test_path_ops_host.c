/*
 * Host unit tests for path_ops (real shipped code).
 * Build: gcc -I../src -o test_path_ops_host test_path_ops_host.c ../src/path_ops.c
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include "path_ops.h"

static void
test_parse_mode(void) {
  mode_t m = 0;
  assert(bfpilot_parse_mode_octal("444", &m) == 0);
  assert((m & 0777) == 0444);
  assert(bfpilot_parse_mode_octal("0755", &m) == 0);
  assert((m & 0777) == 0755);
  assert(bfpilot_parse_mode_octal("999", &m) != 0);
  assert(bfpilot_parse_mode_octal("", &m) != 0);
  puts("PASS parse_mode");
}

static void
test_safe(void) {
  assert(bfpilot_path_is_safe("/data/foo") == 1);
  assert(bfpilot_path_is_safe("/") == 1);
  assert(bfpilot_path_is_safe("/data/file..backup") == 1);
  assert(bfpilot_path_is_safe("/data/.../file") == 1);
  assert(bfpilot_path_is_safe("data/foo") == 0);
  assert(bfpilot_path_is_safe("/data/../etc") == 0);
  assert(bfpilot_path_is_safe("/data/foo/..") == 0);
  assert(bfpilot_path_is_safe("/../data") == 0);
  assert(bfpilot_path_is_safe("/data/bad\nname") == 0);
  puts("PASS path_is_safe");
}

static void
test_byte_tokens(void) {
  char path[] = "/data/\xe6\x97\xa5-\xf3\xb0\x82\x80.bin";
  static const unsigned char expected[] = {
    '/', 'd', 'a', 't', 'a', '/', 0xe6, 0x97, 0xa5, '-', 0x80,
    '.', 'b', 'i', 'n', 0
  };
  assert(bfpilot_path_decode_byte_tokens(path) == 1);
  assert(memcmp(path, expected, sizeof(expected)) == 0);

  char unicode[] = "/data/\xc3\xa9.txt";
  char original[sizeof(unicode)];
  memcpy(original, unicode, sizeof(unicode));
  assert(bfpilot_path_decode_byte_tokens(unicode) == 0);
  assert(memcmp(unicode, original, sizeof(unicode)) == 0);
  puts("PASS reversible filesystem byte tokens");
}

#if defined(_WIN32)
/* Windows host: exercise parse/safe only; full chmod/CI needs POSIX FS. */
static void
test_chmod_and_stat(void) {
  mode_t m = 0;
  assert(bfpilot_parse_mode_octal("444", &m) == 0);
  assert((m & 0777) == 0444);
  /* Unsafe path must be rejected by real shipped helper */
  assert(bfpilot_path_chmod("relative/nope", m) == -EINVAL ||
         bfpilot_path_chmod("relative/nope", m) < 0);
  puts("PASS chmod parse + safe-reject (win host)");
}

static void
test_resolve_ci(void) {
  char out[256];
  assert(bfpilot_path_resolve_ci("../etc", out, sizeof(out)) != 0);
  puts("PASS resolve_ci rejects unsafe (win host)");
}
#else
static void
test_chmod_and_stat(void) {
  char tmpl[] = "/tmp/bfpilot_path_ops_test_XXXXXX";
  char path[128];
  char *d;
  mode_t m;
  struct stat st;

  d = mkdtemp(tmpl);
  assert(d != NULL);
  snprintf(path, sizeof(path), "%s/icon0.png", d);
  {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("x", f);
    fclose(f);
  }
  assert(bfpilot_parse_mode_octal("444", &m) == 0);
  assert(bfpilot_path_chmod(path, m) == 0);
  assert(stat(path, &st) == 0);
  assert((st.st_mode & 0777) == 0444);

  assert(bfpilot_parse_mode_octal("755", &m) == 0);
  assert(bfpilot_path_chmod(path, m) == 0);
  assert(stat(path, &st) == 0);
  assert((st.st_mode & 0777) == 0755);

  unlink(path);
  rmdir(d);
  puts("PASS chmod 444/755 + stat");
}

static void
test_resolve_ci(void) {
  char tmpl[] = "/tmp/bfpilot_ci_XXXXXX";
  char *root = mkdtemp(tmpl);
  char sub[256], nested[256], query[256], out[256];
  assert(root);

  snprintf(sub, sizeof(sub), "%s/AppMeta", root);
  assert(mkdir(sub, 0755) == 0);
  snprintf(nested, sizeof(nested), "%s/Icon0.PNG", sub);
  {
    FILE *f = fopen(nested, "w");
    assert(f);
    fputs("i", f);
    fclose(f);
  }

  snprintf(query, sizeof(query), "%s/appmeta/icon0.png", root);
  assert(bfpilot_path_resolve_ci(query, out, sizeof(out)) == 0);
  {
    struct stat st;
    assert(stat(out, &st) == 0);
  }

  unlink(nested);
  rmdir(sub);
  rmdir(root);
  puts("PASS path_resolve_ci");
}
#endif

int
main(void) {
  test_parse_mode();
  test_safe();
  test_byte_tokens();
  test_chmod_and_stat();
  test_resolve_ci();
  puts("ALL path_ops host tests passed");
  return 0;
}
