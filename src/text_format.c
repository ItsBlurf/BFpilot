/*
 * BFpilot text-editor save formatting helpers.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "text_format.h"

enum newline_style {
  NEWLINE_LF,
  NEWLINE_CRLF,
  NEWLINE_CR
};

static int
has_utf8_bom(const unsigned char *data, size_t size) {
  return data && size >= 3 && data[0] == 0xef && data[1] == 0xbb &&
         data[2] == 0xbf;
}

static enum newline_style
detect_newline_style(const unsigned char *data, size_t size) {
  size_t crlf = 0, lf = 0, cr = 0;
  size_t i = has_utf8_bom(data, size) ? 3 : 0;

  while(i < size) {
    if(data[i] == '\r') {
      if(i + 1 < size && data[i + 1] == '\n') {
        crlf++;
        i += 2;
      } else {
        cr++;
        i++;
      }
    } else if(data[i] == '\n') {
      lf++;
      i++;
    } else {
      i++;
    }
  }

  if(crlf >= lf && crlf >= cr && crlf != 0) return NEWLINE_CRLF;
  if(cr > lf && cr != 0) return NEWLINE_CR;
  return NEWLINE_LF;
}

static int
append_byte(char *out, size_t max_size, size_t *used, unsigned char value) {
  if(*used >= max_size) {
    errno = EFBIG;
    return -1;
  }
  out[(*used)++] = (char)value;
  return 0;
}

int
bfpilot_text_prepare_save(const unsigned char *input, size_t input_size,
                          const unsigned char *current, size_t current_size,
                          size_t max_size, char **output,
                          size_t *output_size) {
  if((input_size != 0 && !input) || (current_size != 0 && !current) ||
     !output || !output_size || max_size == SIZE_MAX) {
    errno = EINVAL;
    return -1;
  }

  char *out = malloc(max_size + 1);
  if(!out) return -1;

  const int keep_bom = has_utf8_bom(current, current_size);
  const enum newline_style style = detect_newline_style(current, current_size);
  size_t used = 0;
  size_t i = has_utf8_bom(input, input_size) ? 3 : 0;

  if(keep_bom &&
     (append_byte(out, max_size, &used, 0xef) != 0 ||
      append_byte(out, max_size, &used, 0xbb) != 0 ||
      append_byte(out, max_size, &used, 0xbf) != 0)) {
    free(out);
    return -1;
  }

  while(i < input_size) {
    unsigned char value = input[i++];
    if(value != '\r' && value != '\n') {
      if(append_byte(out, max_size, &used, value) != 0) {
        free(out);
        return -1;
      }
      continue;
    }

    if(value == '\r' && i < input_size && input[i] == '\n') i++;
    if(style == NEWLINE_CRLF) {
      if(append_byte(out, max_size, &used, '\r') != 0 ||
         append_byte(out, max_size, &used, '\n') != 0) {
        free(out);
        return -1;
      }
    } else if(append_byte(out, max_size, &used,
                          style == NEWLINE_CR ? '\r' : '\n') != 0) {
      free(out);
      return -1;
    }
  }

  out[used] = 0;
  *output = out;
  *output_size = used;
  return 0;
}
