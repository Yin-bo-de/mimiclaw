#pragma once

// Removes only an incomplete UTF-8 sequence at the end of a string.
void mimi_trim_incomplete_utf8_tail(char *text);
