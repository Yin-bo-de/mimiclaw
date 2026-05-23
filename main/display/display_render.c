#include "display/display_render.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define FONT_WIDTH 5
#define FONT_HEIGHT 7
#define FONT_SPACING 1
#define LINE_HEIGHT 10
#define HEADER_TEXT_X 6
#define WEATHER_TEXT_X 8
#define TODO_TEXT_X 166
#define HEADER_TEXT_WIDTH (DISPLAY_RENDER_WIDTH - 12)
#define WEATHER_TEXT_WIDTH (158 - WEATHER_TEXT_X - 4)
#define TODO_TEXT_WIDTH (DISPLAY_RENDER_WIDTH - TODO_TEXT_X - 6)

static const uint8_t FONT_DIGITS[10][FONT_WIDTH] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E},
    {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30},
    {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E},
};

static const uint8_t FONT_LETTERS[26][FONT_WIDTH] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F},
    {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43},
};

static void glyph_for_char(char c, uint8_t glyph[FONT_WIDTH])
{
    memset(glyph, 0, FONT_WIDTH);

    if (c >= '0' && c <= '9') {
        memcpy(glyph, FONT_DIGITS[c - '0'], FONT_WIDTH);
        return;
    }

    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') {
        memcpy(glyph, FONT_LETTERS[c - 'A'], FONT_WIDTH);
        return;
    }

    switch (c) {
    case ' ':
        break;
    case ':':
        glyph[2] = 0x14;
        break;
    case '-':
        glyph[1] = 0x08;
        glyph[2] = 0x08;
        glyph[3] = 0x08;
        break;
    case '/':
        glyph[0] = 0x60;
        glyph[1] = 0x10;
        glyph[2] = 0x08;
        glyph[3] = 0x04;
        glyph[4] = 0x03;
        break;
    case '.':
        glyph[2] = 0x40;
        break;
    case ',':
        glyph[2] = 0x60;
        break;
    case '!':
        glyph[2] = 0x5F;
        break;
    case '?':
        glyph[1] = 0x01;
        glyph[2] = 0x51;
        glyph[3] = 0x09;
        glyph[4] = 0x06;
        break;
    case '+':
        glyph[1] = 0x08;
        glyph[2] = 0x1C;
        glyph[3] = 0x08;
        break;
    case '_':
        glyph[0] = 0x40;
        glyph[1] = 0x40;
        glyph[2] = 0x40;
        glyph[3] = 0x40;
        glyph[4] = 0x40;
        break;
    case '(':
        glyph[2] = 0x1C;
        glyph[3] = 0x22;
        glyph[4] = 0x41;
        break;
    case ')':
        glyph[0] = 0x41;
        glyph[1] = 0x22;
        glyph[2] = 0x1C;
        break;
    default:
        glyph[0] = 0x7F;
        glyph[4] = 0x7F;
        glyph[1] = 0x41;
        glyph[2] = 0x41;
        glyph[3] = 0x41;
        break;
    }
}

static bool framebuffer_is_valid(const uint8_t *framebuffer, size_t framebuffer_len)
{
    return framebuffer && framebuffer_len >= DISPLAY_RENDER_FB_BYTES;
}

static const char *value_or_empty(const char *value)
{
    return (value && value[0] != '\0') ? value : "";
}

static size_t max_chars_for_width(int pixel_width)
{
    if (pixel_width < FONT_WIDTH) {
        return 0;
    }
    return (size_t)((pixel_width + FONT_SPACING) / (FONT_WIDTH + FONT_SPACING));
}

static void truncate_text_for_width(const char *input, char *output, size_t output_size, int pixel_width)
{
    const char *value = value_or_empty(input);
    size_t max_chars = max_chars_for_width(pixel_width);
    size_t input_len = strlen(value);

    if (output_size == 0) {
        return;
    }
    output[0] = '\0';

    if (max_chars == 0) {
        return;
    }
    if (max_chars >= output_size) {
        max_chars = output_size - 1;
    }
    if (input_len <= max_chars) {
        snprintf(output, output_size, "%s", value);
        return;
    }
    if (max_chars <= 3) {
        memset(output, '.', max_chars);
        output[max_chars] = '\0';
        return;
    }

    size_t keep = max_chars - 3;
    memcpy(output, value, keep);
    memcpy(output + keep, "...", 3);
    output[keep + 3] = '\0';
}

static void format_truncated_line(char *output, size_t output_size, const char *label, const char *value, int pixel_width)
{
    const char *safe_label = value_or_empty(label);
    size_t label_len = strlen(safe_label);
    size_t max_chars = max_chars_for_width(pixel_width);

    if (output_size == 0) {
        return;
    }
    output[0] = '\0';

    if (label_len >= max_chars) {
        truncate_text_for_width(safe_label, output, output_size, pixel_width);
        return;
    }

    char truncated_value[64];
    int value_width = pixel_width - (int)(label_len * (FONT_WIDTH + FONT_SPACING));
    truncate_text_for_width(value, truncated_value, sizeof(truncated_value), value_width);
    snprintf(output, output_size, "%s%s", safe_label, truncated_value);
}

void display_render_clear(uint8_t *framebuffer, size_t framebuffer_len, bool white)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }

    memset(framebuffer, white ? 0xFF : 0x00, DISPLAY_RENDER_FB_BYTES);
}

void display_render_draw_pixel(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, bool black)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }
    if (x < 0 || y < 0 || x >= DISPLAY_RENDER_WIDTH || y >= DISPLAY_RENDER_HEIGHT) {
        return;
    }

    size_t index = (size_t)y * (DISPLAY_RENDER_WIDTH / 8) + (size_t)(x / 8);
    uint8_t mask = (uint8_t)(0x80 >> (x % 8));

    if (black) {
        framebuffer[index] &= (uint8_t)~mask;
    } else {
        framebuffer[index] |= mask;
    }
}

void display_render_draw_hline(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int width, bool black)
{
    if (width <= 0) {
        return;
    }

    for (int i = 0; i < width; i++) {
        display_render_draw_pixel(framebuffer, framebuffer_len, x + i, y, black);
    }
}

void display_render_draw_vline(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int height, bool black)
{
    if (height <= 0) {
        return;
    }

    for (int i = 0; i < height; i++) {
        display_render_draw_pixel(framebuffer, framebuffer_len, x, y + i, black);
    }
}

void display_render_draw_rect(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int width, int height, bool black)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    display_render_draw_hline(framebuffer, framebuffer_len, x, y, width, black);
    display_render_draw_hline(framebuffer, framebuffer_len, x, y + height - 1, width, black);
    display_render_draw_vline(framebuffer, framebuffer_len, x, y, height, black);
    display_render_draw_vline(framebuffer, framebuffer_len, x + width - 1, y, height, black);
}

int display_render_draw_text(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, const char *text, bool black)
{
    if (!text || !framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return x;
    }

    int cursor_x = x;
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (cursor_x > DISPLAY_RENDER_WIDTH - FONT_WIDTH) {
            break;
        }

        uint8_t glyph[FONT_WIDTH];
        glyph_for_char(*cursor, glyph);

        for (int col = 0; col < FONT_WIDTH; col++) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if ((glyph[col] & (1 << row)) != 0) {
                    display_render_draw_pixel(framebuffer, framebuffer_len, cursor_x + col, y + row, black);
                }
            }
        }
        cursor_x += FONT_WIDTH + FONT_SPACING;
    }

    return cursor_x;
}

static void draw_text_line(uint8_t *framebuffer, size_t framebuffer_len, int x, int y,
                           int pixel_width, const char *label, const char *value)
{
    char line[64];

    format_truncated_line(line, sizeof(line), label, value, pixel_width);
    display_render_draw_text(framebuffer, framebuffer_len, x, y, line, true);
}

void display_render_dashboard(uint8_t *framebuffer, size_t framebuffer_len, const display_dashboard_data_t *data)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }
    display_render_clear(framebuffer, framebuffer_len, true);

    display_render_draw_rect(framebuffer, framebuffer_len, 0, 0, DISPLAY_RENDER_WIDTH, DISPLAY_RENDER_HEIGHT, true);
    display_render_draw_hline(framebuffer, framebuffer_len, 0, 21, DISPLAY_RENDER_WIDTH, true);
    display_render_draw_vline(framebuffer, framebuffer_len, 158, 21, DISPLAY_RENDER_HEIGHT - 21, true);

    const char *date = (data && data->date && data->date[0] != '\0') ? data->date : "";
    const char *time = (data && data->time && data->time[0] != '\0') ? data->time : "Time syncing...";
    char header_raw[80];
    char header[80];
    if (date[0] != '\0') {
        snprintf(header_raw, sizeof(header_raw), "%s  %s", date, time);
    } else {
        snprintf(header_raw, sizeof(header_raw), "%s", time);
    }
    truncate_text_for_width(header_raw, header, sizeof(header), HEADER_TEXT_WIDTH);
    display_render_draw_text(framebuffer, framebuffer_len, HEADER_TEXT_X, 7, header, true);

    bool has_weather = data && data->weather_city && data->weather_city[0] != '\0';
    draw_text_line(framebuffer, framebuffer_len, WEATHER_TEXT_X, 31, WEATHER_TEXT_WIDTH,
                   "Weather: ", has_weather ? data->weather_city : "set city in chat");
    if (has_weather && data->weather_summary && data->weather_summary[0] != '\0') {
        draw_text_line(framebuffer, framebuffer_len, WEATHER_TEXT_X, 43, WEATHER_TEXT_WIDTH,
                       "", data->weather_summary);
    }

    display_render_draw_text(framebuffer, framebuffer_len, TODO_TEXT_X, 31, "Todos", true);
    display_render_draw_hline(framebuffer, framebuffer_len, TODO_TEXT_X, 41, 42, true);

    if (!data || data->todo_count == 0) {
        draw_text_line(framebuffer, framebuffer_len, TODO_TEXT_X, 52, TODO_TEXT_WIDTH, "", "No todos");
        return;
    }

    size_t count = data->todo_count;
    if (count > 5) {
        count = 5;
    }

    for (size_t i = 0; i < count; i++) {
        const char *todo = value_or_empty(data->todos[i]);
        if (todo[0] == '\0') {
            continue;
        }
        draw_text_line(framebuffer, framebuffer_len, TODO_TEXT_X, 52 + (int)i * LINE_HEIGHT,
                       TODO_TEXT_WIDTH, "- ", todo);
    }
}
