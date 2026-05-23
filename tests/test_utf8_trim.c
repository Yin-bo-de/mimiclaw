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

static void copy_truncates_ascii_and_terminates(void)
{
    char dest[6];

    mimi_copy_string_truncated_utf8(dest, sizeof(dest), "weather");

    assert(strcmp(dest, "weath") == 0);
    assert(valid_utf8(dest));
}

static void copy_preserves_complete_utf8(void)
{
    char dest[16];

    mimi_copy_string_truncated_utf8(dest, sizeof(dest), "天气晴");

    assert(strcmp(dest, "天气晴") == 0);
    assert(valid_utf8(dest));
}

static void copy_trims_partial_utf8_tail(void)
{
    char dest[9];

    mimi_copy_string_truncated_utf8(dest, sizeof(dest), "天气晴朗");

    assert(strcmp(dest, "天气") == 0);
    assert(valid_utf8(dest));
}

static void copy_null_source_writes_empty_string(void)
{
    char dest[8] = "old";

    mimi_copy_string_truncated_utf8(dest, sizeof(dest), NULL);

    assert(strcmp(dest, "") == 0);
}

static void copy_zero_size_destination_is_safe(void)
{
    mimi_copy_string_truncated_utf8(NULL, 0, "天气");
}

int main(void)
{
    trims_partial_three_byte_character();
    keeps_complete_utf8_character();
    keeps_ascii_text();
    copy_truncates_ascii_and_terminates();
    copy_preserves_complete_utf8();
    copy_trims_partial_utf8_tail();
    copy_null_source_writes_empty_string();
    copy_zero_size_destination_is_safe();
    return 0;
}
