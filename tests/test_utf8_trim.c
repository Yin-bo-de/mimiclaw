#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "util/utf8.h"

static bool valid_utf8(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p <= 0x7F) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) return false;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return false;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return false;
            p += 4;
        } else {
            return false;
        }
    }
    return true;
}

static void trims_partial_three_byte_character(void)
{
    char text[] = {'O', 'K', ' ', (char)0xE4, (char)0xB8, '\0'};

    mimi_trim_incomplete_utf8_tail(text);

    assert(strcmp(text, "OK ") == 0);
    assert(valid_utf8(text));
}

static void keeps_complete_utf8_character(void)
{
    char text[] = "OK 中";

    mimi_trim_incomplete_utf8_tail(text);

    assert(strcmp(text, "OK 中") == 0);
    assert(valid_utf8(text));
}

static void keeps_ascii_text(void)
{
    char text[] = "plain ascii";

    mimi_trim_incomplete_utf8_tail(text);

    assert(strcmp(text, "plain ascii") == 0);
    assert(valid_utf8(text));
}

int main(void)
{
    trims_partial_three_byte_character();
    keeps_complete_utf8_character();
    keeps_ascii_text();
    return 0;
}
