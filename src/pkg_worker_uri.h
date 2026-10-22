#pragma once
#include "pkg_source_wire.h"
#include <string.h>
/* Mutates a complete, bounded private protocol request. */
static inline int bfpilot_pkg_parse_uri_request(char *text, char **uri,
                                                char **cid, char **name) {
  if (!text) return 0;
  char *first = strchr(text, '\t');
  if (!first) return 0;
  *first++ = 0;
  char *second = strchr(first, '\t');
  if (!second) return 0;
  *second++ = 0;
  if (!bfps_uri_valid(text) || !first[0] || strlen(first) > 47 ||
      !second[0] || strlen(second) > 255) return 0;
  for (const unsigned char *p = (const unsigned char *)first; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
  const unsigned char *p = (const unsigned char *)second;
  size_t length = strlen(second);
  for (size_t i = 0; i < length;) {
    unsigned char c = p[i];
    if (c < 32 || c == 127) return 0;
    if (c < 128) { i++; continue; }
    size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n || n > length - i) return 0;
    for (size_t j = 1; j < n; j++) if ((p[i+j] & 0xc0) != 0x80) return 0;
    if ((c == 0xe0 && p[i+1] < 0xa0) || (c == 0xed && p[i+1] >= 0xa0) ||
        (c == 0xf0 && p[i+1] < 0x90) || (c == 0xf4 && p[i+1] >= 0x90)) return 0;
    i += n;
  }
  *uri = text; *cid = strcmp(first, "-") ? first : ""; *name = second;
  return 1;
}
