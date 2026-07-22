/*
 * BFpilot text-editor save formatting helpers.
 */

#pragma once

#include <stddef.h>

/*
 * Normalize browser textarea newlines while preserving the existing file's
 * dominant newline convention and UTF-8 BOM. The returned buffer is owned by
 * the caller. Returns 0 on success or -1 with errno set.
 */
int bfpilot_text_prepare_save(const unsigned char *input, size_t input_size,
                              const unsigned char *current,
                              size_t current_size, size_t max_size,
                              char **output, size_t *output_size);
