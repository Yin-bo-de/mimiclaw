#pragma once

#include <stddef.h>

// Removes only an incomplete UTF-8 sequence at the end of a string.
void mimi_trim_incomplete_utf8_tail(char *text);
void mimi_copy_string_truncated_utf8(char *dest, size_t dest_size, const char *src);
