#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "text_format.h"

static void
expect_bytes(const unsigned char *input, size_t input_size,
             const unsigned char *current, size_t current_size,
             const unsigned char *expected, size_t expected_size) {
  char *output = NULL;
  size_t output_size = 0;
  assert(bfpilot_text_prepare_save(input, input_size, current, current_size,
                                   1024, &output, &output_size) == 0);
  assert(output_size == expected_size);
  assert(memcmp(output, expected, expected_size) == 0);
  free(output);
}

int
main(void) {
  static const unsigned char lf_current[] = "one\ntwo\n";
  static const unsigned char mixed_input[] = "a\r\nb\rc\n";
  static const unsigned char lf_expected[] = "a\nb\nc\n";
  expect_bytes(mixed_input, sizeof(mixed_input) - 1,
               lf_current, sizeof(lf_current) - 1,
               lf_expected, sizeof(lf_expected) - 1);

  static const unsigned char crlf_current[] = "one\r\ntwo\r\n";
  static const unsigned char crlf_expected[] = "a\r\nb\r\nc\r\n";
  expect_bytes(mixed_input, sizeof(mixed_input) - 1,
               crlf_current, sizeof(crlf_current) - 1,
               crlf_expected, sizeof(crlf_expected) - 1);

  static const unsigned char cr_current[] = "one\rtwo\r";
  static const unsigned char cr_expected[] = "a\rb\rc\r";
  expect_bytes(mixed_input, sizeof(mixed_input) - 1,
               cr_current, sizeof(cr_current) - 1,
               cr_expected, sizeof(cr_expected) - 1);

  static const unsigned char bom_current[] = {
    0xef, 0xbb, 0xbf, 'x', '\r', '\n'
  };
  static const unsigned char bom_input[] = {
    0xef, 0xbb, 0xbf, 'y', '\n'
  };
  static const unsigned char bom_expected[] = {
    0xef, 0xbb, 0xbf, 'y', '\r', '\n'
  };
  expect_bytes(bom_input, sizeof(bom_input), bom_current, sizeof(bom_current),
               bom_expected, sizeof(bom_expected));

  char *output = NULL;
  size_t output_size = 0;
  errno = 0;
  assert(bfpilot_text_prepare_save(
      (const unsigned char *)"a\nb", 3, crlf_current,
      sizeof(crlf_current) - 1, 3, &output, &output_size) == -1);
  assert(errno == EFBIG);
  assert(output == NULL);

  puts("text format host tests passed");
  return 0;
}
