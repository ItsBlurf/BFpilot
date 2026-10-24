#pragma once
#include <stdint.h>

/* SDK firmware values use packed decimal digits, e.g. 0x13600000 for 13.60. */
static inline int bfpilot_firmware_major_from_sdk(uint32_t version) {
  unsigned int major = version >> 24;
  unsigned int minor = (version >> 16) & 0xffU;
  if((major >> 4) > 9 || (major & 15U) > 9 ||
     (minor >> 4) > 9 || (minor & 15U) > 9) return 0;
  return (int)((major >> 4) * 10U + (major & 15U));
}
