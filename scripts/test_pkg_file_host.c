#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pkg_file.h"

#define HEADER_SIZE 0x440U

static void
put_be32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value >> 24);
  p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8);
  p[3] = (unsigned char)value;
}

static void
put_be64(unsigned char *p, uint64_t value) {
  for(int i = 7; i >= 0; i--) {
    p[i] = (unsigned char)value;
    value >>= 8;
  }
}

static void
make_header(unsigned char header[HEADER_SIZE]) {
  static const char content_id[] =
      "EP4433-CUSA00265_00-MINECRAFTPS40000";
  memset(header, 0, HEADER_SIZE);
  memcpy(header, "\x7f" "CNT", 4);
  memcpy(header + 0x40, content_id, sizeof(content_id) - 1);
  put_be32(header + 0x74, 0x1a);
  put_be32(header + 0x78, 0x80000000U);
  put_be64(header + 0x430, HEADER_SIZE);
}

int
main(void) {
  unsigned char header[HEADER_SIZE];
  bfpilot_pkg_file_info_t info;
  make_header(header);
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE,
                                     &info) == 0);
  assert(!strcmp(info.content_id,
                 "EP4433-CUSA00265_00-MINECRAFTPS40000"));
  assert(!strcmp(info.title_id, "CUSA00265"));
  assert(info.format == BFPILOT_PKG_FORMAT_CNT);
  assert(!bfpilot_pkg_file_is_system(&info));

  memcpy(header + 0x40, "UP9000-NPXS40000_00-SYSTEMPACKAGE000", 37);
  memset(header + 0x40 + 37, 0, 0x30 - 37);
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE,
                                     &info) == 0);
  assert(bfpilot_pkg_file_is_system(&info));
  assert(info.size == HEADER_SIZE);
  assert(info.content_type == 0x1a);
  assert(info.content_flags == 0x80000000U);

  memset(header, 0xa5, sizeof(header));
  memcpy(header, "\x7f" "FIH", 4);
  assert(bfpilot_pkg_header_validate(header, sizeof(header), 37633547,
                                     &info) == 0);
  assert(info.format == BFPILOT_PKG_FORMAT_FIH);
  assert(info.size == 37633547);
  assert(info.content_id[0] == 0 && info.title_id[0] == 0);
  assert(!bfpilot_pkg_file_is_system(&info));
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE - 1,
                                     &info) != 0);

  header[0] = 0;
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE,
                                     &info) != 0);
  make_header(header);
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE + 1,
                                     &info) != 0);
  make_header(header);
  header[0x40] = '\n';
  assert(bfpilot_pkg_header_validate(header, sizeof(header), HEADER_SIZE,
                                     &info) != 0);
  assert(bfpilot_pkg_header_validate(header, HEADER_SIZE - 1, HEADER_SIZE,
                                     &info) != 0);
  puts("PKG header host tests passed");
  return 0;
}
