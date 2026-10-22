#ifndef BFPILOT_URL_CODEC_H
#define BFPILOT_URL_CODEC_H
#include <stddef.h>
/* Strict, bounded decoding; invalid escapes, NUL and truncation are errors. */
int bfpilot_url_decode(char *out, size_t capacity, const char *input, size_t length);
/* Decode a scalar form/query field, converting private path byte tokens before
 * enforcing its final capacity. Returns 1 only for a complete matching field. */
int bfpilot_form_arg(const char *body, const char *name, char *out, size_t capacity);
#endif
