#include "archive_batch_codec.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bfab_item_t *items, *restored;
static unsigned char *wire, *copy;
static unsigned controls;
static bfab_job_t job(unsigned count) {
  bfab_job_t j = {0};
  strcpy(j.id, "owned-batch-23"); strcpy(j.password, " pwd=中文 ");
  j.items = items; j.count = count; j.state = BFAB_PREPARED;
  j.revision = 1; j.created = 1792700000; j.nice = -5;
  memset(items, 0, BFAB_MAX_ITEMS * sizeof(*items));
  for (unsigned i = 0; i < count; i++) {
    snprintf(items[i].source, sizeof(items[i].source), "/data/档案-%u.7z", i);
    snprintf(items[i].destination, sizeof(items[i].destination), "/mnt/usb0/out-%u", i);
  }
  return j;
}
static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t c = UINT32_MAX;
  while (n--) {
    c ^= *p++;
    for (unsigned i = 0; i < 8; i++) c = c >> 1 ^ (0xedb88320U & (0U - (c & 1)));
  }
  return ~c;
}
static void put32(unsigned char *p, uint32_t n) {
  for (int i = 3; i >= 0; i--, n >>= 8) p[i] = (unsigned char)n;
}
static void checksum(unsigned char *p, size_t n) { put32(p + n - 4, crc32(p, n - 4)); }
static void roundtrip(const bfab_job_t *j) {
  size_t n = 0;
  assert(!bfab_manifest_encode(j, wire, BFAB_MANIFEST_MAX, &n));
  bfab_job_t out;
  assert(!bfab_manifest_decode(wire, n, &out, restored, BFAB_MAX_ITEMS));
  assert(out.count == j->count && out.next == j->next && out.state == j->state &&
    out.flags == j->flags && out.revision == j->revision && out.created == j->created &&
    out.succeeded == j->succeeded && out.failed == j->failed && out.error == j->error &&
    out.nice == j->nice && !strcmp(out.password, j->password) && !strcmp(out.id, j->id));
  for (unsigned i = 0; i < j->count; i++)
    assert(!strcmp(restored[i].source, j->items[i].source) &&
      !strcmp(restored[i].destination, j->items[i].destination) &&
      restored[i].state == j->items[i].state && restored[i].result == j->items[i].result);
}
static bfab_event_t event(const bfab_job_t *j, unsigned kind, unsigned index, int result) {
  bfab_event_t e = {0}; strcpy(e.id, j->id); e.sequence = j->revision + 1;
  e.kind = kind; e.index = index; e.result = result; return e;
}
static void apply(bfab_job_t *j, unsigned kind, unsigned index, int result) {
  bfab_event_t e = event(j, kind, index, result), out;
  unsigned char b[BFAB_EVENT_SIZE];
  assert(!bfab_event_encode(&e, b, sizeof(b)));
  assert(!bfab_event_decode(b, sizeof(b), &out));
  assert(!bfab_event_apply(j, &out));
}
static void rejects_unchanged(bfab_job_t *j, bfab_event_t *e) {
  bfab_job_t before = *j;
  memcpy(restored, j->items, j->count * sizeof(*items));
  assert(bfab_event_apply(j, e) < 0);
  assert(!memcmp(&before, j, sizeof(before)) &&
         !memcmp(restored, j->items, j->count * sizeof(*items)));
}
int main(void) {
  items = calloc(BFAB_MAX_ITEMS, sizeof(*items));
  restored = calloc(BFAB_MAX_ITEMS, sizeof(*items));
  wire = malloc(BFAB_MANIFEST_MAX + 1); copy = malloc(BFAB_MANIFEST_MAX + 1);
  assert(items && restored && wire && copy);
  bfab_job_t j = job(3), out;
  roundtrip(&j); controls++;
  size_t n = 0; assert(!bfab_manifest_encode(&j, wire, BFAB_MANIFEST_MAX, &n));
  for (size_t i = 0; i < n; i++) {
    memset(&out, 0xa5, sizeof(out));
    assert(bfab_manifest_decode(wire, i, &out, restored, BFAB_MAX_ITEMS) < 0);
    bfab_job_t zero = {0}; assert(!memcmp(&out, &zero, sizeof(out)));
    memcpy(copy, wire, n); copy[i] ^= 0x80;
    assert(bfab_manifest_decode(copy, n, &out, restored, BFAB_MAX_ITEMS) < 0);
  }
  controls++;
  assert(bfab_manifest_decode(wire, n, &out, restored, 2) < 0);
  wire[n] = 0; assert(bfab_manifest_decode(wire, n + 1, &out, restored, BFAB_MAX_ITEMS) < 0);
  for (unsigned off = 8; off < 64; off += 4) {
    memcpy(copy, wire, n); put32(copy + off, UINT32_MAX); checksum(copy, n);
    /* -1 nice, arbitrary creation time and high revision bits are legitimate. */
    int rc = bfab_manifest_decode(copy, n, &out, restored, BFAB_MAX_ITEMS);
    assert((off == 40 || off >= 48) ? rc == 0 : rc < 0);
  }
  memcpy(copy, wire, n); put32(copy + 64, UINT32_MAX); checksum(copy, n);
  assert(bfab_manifest_decode(copy, n, &out, restored, BFAB_MAX_ITEMS) < 0);
  controls++;
  /* Reject before writing even a byte when the output does not fit. */
  memset(copy, 0xa5, n + 1); size_t written = 99;
  assert(bfab_manifest_encode(&j, copy, n - 1, &written) == -EMSGSIZE && !written);
  for (size_t i = 0; i <= n; i++) assert(copy[i] == 0xa5);
  controls++;
  bfab_event_t e = event(&j, BFAB_FINISH, 0, 0); rejects_unchanged(&j, &e);
  e = event(&j, BFAB_BEGIN, 1, 0); rejects_unchanged(&j, &e);
  e = event(&j, BFAB_BEGIN, 0, 0); strcpy(e.id, "foreign"); rejects_unchanged(&j, &e);
  e = event(&j, BFAB_BEGIN, 0, 0); e.sequence--; rejects_unchanged(&j, &e);
  apply(&j, BFAB_BEGIN, 0, 0);
  e = event(&j, BFAB_BEGIN, 0, 0); rejects_unchanged(&j, &e);
  apply(&j, BFAB_FINISH, 0, 0); roundtrip(&j);
  apply(&j, BFAB_BEGIN, 1, 0); apply(&j, BFAB_FINISH, 1, EACCES);
  assert(j.next == 2 && j.succeeded == 1 && j.failed == 1 && j.state == BFAB_RUNNING);
  roundtrip(&j);
  apply(&j, BFAB_BEGIN, 2, 0); apply(&j, BFAB_FINISH, 2, 0);
  assert(j.state == BFAB_FINISHED && j.next == 3); roundtrip(&j);
  e = event(&j, BFAB_CANCEL, UINT32_MAX, 0); rejects_unchanged(&j, &e); controls++;
  /* Cancellation before dispatch, between items, and during current work. */
  for (unsigned mode = 0; mode < 3; mode++) {
    j = job(3);
    if (mode) { apply(&j, BFAB_BEGIN, 0, 0); apply(&j, BFAB_FINISH, 0, 0); }
    if (mode == 2) apply(&j, BFAB_BEGIN, 1, 0);
    apply(&j, BFAB_CANCEL, UINT32_MAX, 0);
    e = event(&j, BFAB_BEGIN, j.next, 0); rejects_unchanged(&j, &e);
    if (mode == 2) {
      assert(j.state == BFAB_RUNNING); roundtrip(&j);
      apply(&j, BFAB_FINISH, 1, ECANCELED);
    }
    assert(j.state == BFAB_CANCELLED); roundtrip(&j);
    for (unsigned i = j.next; i < j.count; i++) assert(j.items[i].state == BFAB_PENDING);
  }
  controls++;
  j = job(1); apply(&j, BFAB_BEGIN, 0, 0); apply(&j, BFAB_CANCEL, UINT32_MAX, 0);
  apply(&j, BFAB_FINISH, 0, 0); assert(j.state == BFAB_CANCELLED && j.succeeded == 1);
  roundtrip(&j); controls++;
  j = job(2); apply(&j, BFAB_BEGIN, 0, 0); apply(&j, BFAB_HALT, UINT32_MAX, EIO);
  assert(j.items[0].state == BFAB_ACTIVE && !j.next && j.state == BFAB_ERROR);
  roundtrip(&j); e = event(&j, BFAB_FINISH, 0, 0); rejects_unchanged(&j, &e); controls++;
  j = job(2); j.revision = UINT64_MAX;
  e = event(&j, BFAB_BEGIN, 0, 0); rejects_unchanged(&j, &e); controls++;
  j = job(1); e = event(&j, BFAB_BEGIN, 0, 0);
  unsigned char record[BFAB_EVENT_SIZE]; assert(!bfab_event_encode(&e, record, sizeof(record)));
  for (size_t i = 0; i < sizeof(record); i++) {
    bfab_event_t decoded;
    assert(bfab_event_decode(record, i, &decoded) < 0);
    memcpy(copy, record, sizeof(record)); copy[i] ^= 0x80;
    assert(bfab_event_decode(copy, sizeof(record), &decoded) < 0);
  }
  memcpy(copy, record, sizeof(record)); copy[92] = 1; checksum(copy, sizeof(record));
  assert(bfab_event_decode(copy, sizeof(record), &e) < 0);
  memcpy(copy, record, sizeof(record)); copy[8 + strlen(j.id) + 1] = 'x'; checksum(copy, sizeof(record));
  assert(bfab_event_decode(copy, sizeof(record), &e) < 0); controls++;
  const char *invalid[] = {"/user/app/a", "/smb/85328221df355b81/share/a", "/data/../a",
    "/data/./a", "/data//a", "/data/", "/database/a", "/mnt/usb8/a", "/mnt/usb01/a",
    "/data/a\nb", "/data/a\177b"};
  for (unsigned i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
    j = job(1); strcpy(items[0].source, invalid[i]); assert(bfab_validate(&j) < 0);
  }
  j = job(1); strcpy(items[0].source, "/data/raw-\xff.7z");
  strcpy(items[0].destination, "/data"); roundtrip(&j);
  j.count = BFAB_MAX_ITEMS + 1; assert(bfab_validate(&j) < 0);
  j = job(1); memset(j.password, 'a', sizeof(j.password)); assert(bfab_validate(&j) < 0);
  j = job(1); memset(items[0].source, 'a', sizeof(items[0].source)); assert(bfab_validate(&j) < 0);
  controls++;
  /* Actual maximum selection and near-maximum byte lengths; no small model. */
  j = job(BFAB_MAX_ITEMS);
  for (unsigned i = 0; i < j.count; i++) {
    memset(items[i].source + strlen(items[i].source), 's',
           sizeof(items[i].source) - strlen(items[i].source) - 1);
    memset(items[i].destination + strlen(items[i].destination), 'd',
           sizeof(items[i].destination) - strlen(items[i].destination) - 1);
  }
  roundtrip(&j);
  for (unsigned i = 0; i < j.count; i++) {
    apply(&j, BFAB_BEGIN, i, 0); apply(&j, BFAB_FINISH, i, (i % 3) ? 0 : EIO);
  }
  assert(j.next == BFAB_MAX_ITEMS && j.failed == 683 && j.succeeded == 1365);
  roundtrip(&j); controls++;
  /* Deep malformed inputs with valid CRC, deterministic and sanitizer-friendly. */
  j = job(3); assert(!bfab_manifest_encode(&j, wire, BFAB_MANIFEST_MAX, &n));
  uint32_t seed = 23;
  for (unsigned trial = 0; trial < 12000; trial++) {
    memcpy(copy, wire, n);
    seed = seed * 1664525U + 1013904223U; size_t pos = seed % (n - 4);
    seed = seed * 1664525U + 1013904223U; copy[pos] ^= (unsigned char)(1U + seed % 255U);
    checksum(copy, n);
    int rc = bfab_manifest_decode(copy, n, &out, restored, BFAB_MAX_ITEMS);
    if (!rc) assert(!bfab_validate(&out));
  }
  controls++;
  printf("archive batch codec: %u control groups passed (2048 items, 12000 deep mutations)\n", controls);
  free(copy); free(wire); free(restored); free(items);
  return 0;
}
