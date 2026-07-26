#pragma once

#include <stddef.h>

// Converts an MDX title from its native CP932/Shift-JIS encoding to UTF-8.
// Returns 0 on success and -1 if the destination is too small or conversion initialization fails.
int mdx_title_to_utf8(const char* shift_jis, char* utf8, size_t utf8_size);
