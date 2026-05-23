#include "util/utf8.h"

#include <stddef.h>
#include <string.h>

void mimi_trim_incomplete_utf8_tail(char *text)
{
    if (!text) {
        return;
    }

    size_t len = strlen(text);
    if (len == 0) {
        return;
    }

    size_t start = len;
    while (start > 0 && (((unsigned char)text[start - 1] & 0xC0) == 0x80)) {
        start--;
    }

    unsigned char lead = (unsigned char)text[start ? start - 1 : 0];
    size_t char_start = start ? start - 1 : 0;
    size_t expected = 1;

    if (lead <= 0x7F) {
        return;
    } else if ((lead & 0xE0) == 0xC0) {
        expected = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        expected = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        expected = 4;
    } else {
        text[char_start] = '\0';
        return;
    }

    if (len - char_start < expected) {
        text[char_start] = '\0';
    }
}
