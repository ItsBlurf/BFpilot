#include "firmware.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  unsigned int cases = 0;
  for(unsigned int major = 1; major <= 99; major++) {
    for(unsigned int minor = 0; minor <= 99; minor++) {
      uint32_t encoded = ((major / 10U) << 28) | ((major % 10U) << 24) |
                         ((minor / 10U) << 20) | ((minor % 10U) << 16);
      assert(bfpilot_firmware_major_from_sdk(encoded) == (int)major);
      cases++;
    }
  }
  assert(bfpilot_firmware_major_from_sdk(0) == 0);
  assert(bfpilot_firmware_major_from_sdk(0xffffffffU) == 0);
  assert(bfpilot_firmware_major_from_sdk(0x1a600000U) == 0);
  assert(bfpilot_firmware_major_from_sdk(0x13f00000U) == 0);
  assert(bfpilot_firmware_major_from_sdk(0x13600000U) == 13);
  assert(bfpilot_firmware_major_from_sdk(0x11600000U) == 11);
  assert(bfpilot_firmware_major_from_sdk(0x14100000U) == 14);
  printf("Firmware decoding: %u version cases and 7 boundaries passed\n", cases);
  return 0;
}
