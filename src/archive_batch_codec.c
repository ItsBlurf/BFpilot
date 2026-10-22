/* Portable archive queue format. CRC detects corruption, not malicious edits;
   the coordinator must keep the containing directory and files private. */
#include "archive_batch_codec.h"
#include <errno.h>
#include <string.h>

static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}
static uint64_t get64(const unsigned char *p) {
  return (uint64_t)get32(p) << 32 | get32(p + 4);
}
static void put32(unsigned char *p, uint32_t n) {
  for (int i = 3; i >= 0; i--, n >>= 8) p[i] = (unsigned char)n;
}
static void put64(unsigned char *p, uint64_t n) {
  put32(p, (uint32_t)(n >> 32)); put32(p + 4, (uint32_t)n);
}
static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t crc = UINT32_MAX;
  while (n--) {
    crc ^= *p++;
    for (unsigned i = 0; i < 8; i++)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
static int text(const char *p, size_t capacity, int empty) {
  const char *end = memchr(p, 0, capacity);
  if (!end || (!empty && end == p)) return 0;
  for (; p != end; p++)
    if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return 0;
  return 1;
}
static int id_valid(const char *p) {
  if (!text(p, BFAB_ID_SIZE, 0)) return 0;
  for (; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
  return 1;
}
static int path_valid(const char *p) {
  if (!text(p, BFAB_PATH_SIZE, 0)) return 0;
  int allowed = !strncmp(p, "/data", 5) && (!p[5] || p[5] == '/');
  if (!allowed && strlen(p) >= 9 && !strncmp(p, "/mnt/", 5) &&
      (!strncmp(p + 5, "usb", 3) || !strncmp(p + 5, "ext", 3)) &&
      p[8] >= '0' && p[8] <= '7' && (!p[9] || p[9] == '/')) allowed = 1;
  if (!allowed) return 0;
  const char *segment = p + 1;
  for (const char *at = segment;; at++) {
    if (*at && *at != '/') continue;
    size_t n = (size_t)(at - segment);
    if (!n || (n == 1 && segment[0] == '.') ||
        (n == 2 && segment[0] == '.' && segment[1] == '.')) return 0;
    if (!*at) return 1;
    segment = at + 1;
  }
}
static int job_header_valid(const bfab_job_t *j) {
  if (!j || !j->items || !id_valid(j->id) ||
      !text(j->password, BFAB_PASSWORD_SIZE, 1) || !j->count ||
      j->count > BFAB_MAX_ITEMS || j->next > j->count || j->threads > 8 ||
      j->nice < -20 || j->nice > 20 || !j->revision ||
      (j->flags & ~(BFAB_MERGE | BFAB_CLEANUP | BFAB_CANCEL_REQUESTED)) ||
      j->state < BFAB_PREPARED || j->state > BFAB_ERROR ||
      j->succeeded > j->next || j->failed != j->next - j->succeeded)
    return -EINVAL;
  return 0;
}
int bfab_validate(const bfab_job_t *j) {
  if (job_header_valid(j)) return -EINVAL;
  uint32_t good = 0, bad = 0;
  int active = 0;
  for (uint32_t i = 0; i < j->count; i++) {
    const bfab_item_t *it = &j->items[i];
    if (!path_valid(it->source) || !path_valid(it->destination) ||
        !strcmp(it->source, it->destination)) return -EINVAL;
    if (i < j->next) {
      if (it->state == BFAB_DONE && !it->result) good++;
      else if (it->state == BFAB_FAILED && it->result) bad++;
      else return -EINVAL;
    } else {
      if (it->result) return -EINVAL;
      if (i == j->next && it->state == BFAB_ACTIVE) active = 1;
      else if (it->state != BFAB_PENDING) return -EINVAL;
    }
  }
  if (good != j->succeeded || bad != j->failed) return -EINVAL;
  if (j->state != BFAB_ERROR && j->error) return -EINVAL;
  switch (j->state) {
  case BFAB_PREPARED:
    if (j->next || active || (j->flags & BFAB_CANCEL_REQUESTED)) return -EINVAL;
    break;
  case BFAB_RUNNING:
    if (j->next == j->count ||
        ((j->flags & BFAB_CANCEL_REQUESTED) && !active)) return -EINVAL;
    break;
  case BFAB_FINISHED:
    if (j->next != j->count || (j->flags & BFAB_CANCEL_REQUESTED)) return -EINVAL;
    break;
  case BFAB_CANCELLED:
    if (active || !(j->flags & BFAB_CANCEL_REQUESTED)) return -EINVAL;
    break;
  case BFAB_ERROR:
    if (!j->error) return -EINVAL;
    break;
  }
  return 0;
}
int bfab_manifest_encode(const bfab_job_t *j, void *buffer, size_t capacity,
                         size_t *written) {
  if (written) *written = 0;
  if (!buffer || !written || bfab_validate(j)) return -EINVAL;
  size_t n = 76 + strlen(j->id) + strlen(j->password);
  for (uint32_t i = 0; i < j->count; i++)
    n += 16 + strlen(j->items[i].source) + strlen(j->items[i].destination);
  if (n > capacity) return -EMSGSIZE;
  unsigned char *out = buffer, *p = out + 8;
  memcpy(out, "BFBATCH1", 8);
  const uint32_t fields[] = {1, j->flags, j->count, j->next, j->succeeded,
    j->failed, j->state, j->threads, (uint32_t)j->nice, (uint32_t)j->error};
  for (unsigned i = 0; i < 10; i++, p += 4) put32(p, fields[i]);
  put64(p, j->revision); p += 8; put64(p, j->created); p += 8;
  size_t len = strlen(j->id);
  put32(p, (uint32_t)len); p += 4; memcpy(p, j->id, len); p += len;
  len = strlen(j->password);
  put32(p, (uint32_t)len); p += 4; memcpy(p, j->password, len); p += len;
  for (uint32_t i = 0; i < j->count; i++) {
    const bfab_item_t *it = &j->items[i];
    put32(p, it->state); p += 4; put32(p, (uint32_t)it->result); p += 4;
    len = strlen(it->source);
    put32(p, (uint32_t)len); p += 4; memcpy(p, it->source, len); p += len;
    len = strlen(it->destination);
    put32(p, (uint32_t)len); p += 4; memcpy(p, it->destination, len); p += len;
  }
  put32(p, crc32(out, (size_t)(p - out)));
  *written = n;
  return 0;
}
static int read_text(const unsigned char **cursor, const unsigned char *end,
                     char *out, size_t capacity) {
  const unsigned char *p = *cursor;
  if ((size_t)(end - p) < 4) return -EPROTO;
  size_t n = get32(p); p += 4;
  if (n >= capacity || n > (size_t)(end - p) || memchr(p, 0, n)) return -EPROTO;
  memcpy(out, p, n); out[n] = 0; *cursor = p + n;
  return 0;
}
int bfab_manifest_decode(const void *buffer, size_t size, bfab_job_t *out,
                         bfab_item_t *items, size_t capacity) {
  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  if (!buffer || !items || size < 76 || size > BFAB_MANIFEST_MAX) return -EPROTO;
  const unsigned char *in = buffer, *end = in + size - 4;
  if (memcmp(in, "BFBATCH1", 8) || get32(in + 8) != 1 ||
      get32(end) != crc32(in, size - 4)) return -EPROTO;
  bfab_job_t j = {0};
  j.flags = get32(in + 12); j.count = get32(in + 16);
  j.next = get32(in + 20); j.succeeded = get32(in + 24);
  j.failed = get32(in + 28); j.state = get32(in + 32);
  j.threads = get32(in + 36); j.nice = (int32_t)get32(in + 40);
  j.error = (int32_t)get32(in + 44);
  j.revision = get64(in + 48); j.created = get64(in + 56);
  if (!j.count || j.count > BFAB_MAX_ITEMS || j.count > capacity) return -EPROTO;
  const unsigned char *p = in + 64;
  if (read_text(&p, end, j.id, sizeof(j.id)) ||
      read_text(&p, end, j.password, sizeof(j.password))) return -EPROTO;
  memset(items, 0, j.count * sizeof(*items)); j.items = items;
  for (uint32_t i = 0; i < j.count; i++) {
    if ((size_t)(end - p) < 8) return -EPROTO;
    items[i].state = get32(p); p += 4;
    items[i].result = (int32_t)get32(p); p += 4;
    if (read_text(&p, end, items[i].source, sizeof(items[i].source)) ||
        read_text(&p, end, items[i].destination, sizeof(items[i].destination)))
      return -EPROTO;
  }
  if (p != end || bfab_validate(&j)) return -EPROTO;
  *out = j;
  return 0;
}
static int event_valid(const bfab_event_t *e) {
  return e && id_valid(e->id) && e->sequence &&
    ((e->kind == BFAB_BEGIN && e->index < BFAB_MAX_ITEMS && !e->result) ||
     (e->kind == BFAB_FINISH && e->index < BFAB_MAX_ITEMS) ||
     (e->kind == BFAB_CANCEL && e->index == UINT32_MAX && !e->result) ||
     (e->kind == BFAB_HALT && e->index == UINT32_MAX && e->result));
}
int bfab_event_encode(const bfab_event_t *e, void *buffer, size_t capacity) {
  if (!buffer || !event_valid(e)) return -EINVAL;
  if (capacity < BFAB_EVENT_SIZE) return -EMSGSIZE;
  unsigned char *p = buffer;
  memset(p, 0, BFAB_EVENT_SIZE); memcpy(p, "BFABEVT1", 8);
  memcpy(p + 8, e->id, strlen(e->id)); put64(p + 72, e->sequence);
  put32(p + 80, e->kind); put32(p + 84, e->index);
  put32(p + 88, (uint32_t)e->result); /* +92 reserved, must remain zero. */
  put32(p + 96, crc32(p, 96));
  return 0;
}
int bfab_event_decode(const void *buffer, size_t size, bfab_event_t *out) {
  if (!out) return -EINVAL;
  memset(out, 0, sizeof(*out));
  if (!buffer || size != BFAB_EVENT_SIZE) return -EPROTO;
  const unsigned char *p = buffer;
  if (memcmp(p, "BFABEVT1", 8) || get32(p + 92) ||
      get32(p + 96) != crc32(p, 96)) return -EPROTO;
  bfab_event_t e = {0};
  memcpy(e.id, p + 8, sizeof(e.id)); e.sequence = get64(p + 72);
  e.kind = get32(p + 80); e.index = get32(p + 84);
  e.result = (int32_t)get32(p + 88);
  if (!event_valid(&e)) return -EPROTO;
  /* Reject hidden suffix bytes instead of accepting two encodings for an ID. */
  for (size_t i = strlen(e.id); i < sizeof(e.id); i++)
    if (p[8 + i]) return -EPROTO;
  *out = e;
  return 0;
}
int bfab_event_check(const bfab_job_t *j, const bfab_event_t *e) {
  /* A decoded/validated manifest is the owner invariant. Inspect only the
     current item here: a 2048-item queue must not rescan 4 MiB per event. */
  if (job_header_valid(j) || !event_valid(e)) return -EINVAL;
  if (strcmp(j->id, e->id) || j->revision == UINT64_MAX ||
      e->sequence != j->revision + 1) return -EPROTO;
  if (j->state == BFAB_FINISHED || j->state == BFAB_CANCELLED ||
      j->state == BFAB_ERROR) return -EALREADY;
  switch (e->kind) {
  case BFAB_BEGIN:
    return e->index == j->next && j->next < j->count &&
      j->items[j->next].state == BFAB_PENDING &&
      !(j->flags & BFAB_CANCEL_REQUESTED) ? 0 : -EINVAL;
  case BFAB_FINISH:
    return e->index == j->next && j->next < j->count &&
      j->items[j->next].state == BFAB_ACTIVE ? 0 : -EINVAL;
  case BFAB_CANCEL:
    return (j->flags & BFAB_CANCEL_REQUESTED) ? -EALREADY : 0;
  case BFAB_HALT: return 0;
  }
  return -EINVAL;
}
int bfab_event_apply(bfab_job_t *j, const bfab_event_t *e) {
  int rc = bfab_event_check(j, e);
  if (rc) return rc;
  switch (e->kind) {
  case BFAB_BEGIN:
    j->items[j->next].state = BFAB_ACTIVE; j->state = BFAB_RUNNING;
    break;
  case BFAB_FINISH:
    j->items[j->next].result = e->result;
    j->items[j->next].state = e->result ? BFAB_FAILED : BFAB_DONE;
    if (e->result) j->failed++; else j->succeeded++;
    j->next++;
    j->state = (j->flags & BFAB_CANCEL_REQUESTED) ? BFAB_CANCELLED :
      j->next == j->count ? BFAB_FINISHED : BFAB_RUNNING;
    break;
  case BFAB_CANCEL:
    j->flags |= BFAB_CANCEL_REQUESTED;
    if (j->next == j->count || j->items[j->next].state != BFAB_ACTIVE)
      j->state = BFAB_CANCELLED;
    break;
  case BFAB_HALT:
    j->state = BFAB_ERROR; j->error = e->result;
    break;
  }
  j->revision = e->sequence;
  return 0;
}
