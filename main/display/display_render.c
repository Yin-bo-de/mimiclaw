#include "display/display_render.h"
#include "display/display_font_zh12.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define ASCII_FONT_WIDTH 5
#define ASCII_FONT_HEIGHT 7
#define ASCII_FONT_SPACING 1
#define ZH_FONT_WIDTH DISPLAY_ZH12_WIDTH
#define ZH_FONT_HEIGHT DISPLAY_ZH12_HEIGHT
#define ZH_FONT_SPACING 1
#define LINE_HEIGHT 14
#define HEADER_TEXT_X 6
#define WEATHER_TEXT_X 8
#define TODO_TEXT_X 166
#define HEADER_TEXT_WIDTH (DISPLAY_RENDER_WIDTH - 12)
#define WEATHER_TEXT_WIDTH (158 - WEATHER_TEXT_X - 4)
#define TODO_TEXT_WIDTH (DISPLAY_RENDER_WIDTH - TODO_TEXT_X - 6)
#define HEADER_REGION_X 1
#define HEADER_REGION_Y 1
#define HEADER_REGION_WIDTH (DISPLAY_RENDER_WIDTH - 2)
#define HEADER_REGION_HEIGHT 20
#define WEATHER_REGION_X 1
#define WEATHER_REGION_Y 22
#define WEATHER_REGION_WIDTH 157
#define WEATHER_REGION_HEIGHT (DISPLAY_RENDER_HEIGHT - 23)
#define TODOS_REGION_X 159
#define TODOS_REGION_Y 22
#define TODOS_REGION_WIDTH (DISPLAY_RENDER_WIDTH - 160)
#define TODOS_REGION_HEIGHT (DISPLAY_RENDER_HEIGHT - 23)
#define DISPLAY_COLOR_BLACK 0x00
#define DISPLAY_COLOR_WHITE 0x01

static const uint8_t FONT_DIGITS[10][ASCII_FONT_WIDTH] = {
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

static const uint8_t FONT_LETTERS[26][ASCII_FONT_WIDTH] = {
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

static void glyph_for_char(char c, uint8_t glyph[ASCII_FONT_WIDTH])
{
    memset(glyph, 0, ASCII_FONT_WIDTH);

    if (c >= '0' && c <= '9') {
        memcpy(glyph, FONT_DIGITS[c - '0'], ASCII_FONT_WIDTH);
        return;
    }

    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') {
        memcpy(glyph, FONT_LETTERS[c - 'A'], ASCII_FONT_WIDTH);
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

typedef struct {
    uint32_t codepoint;
    size_t advance;
    bool valid;
} decoded_codepoint_t;

static bool is_utf8_continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

static decoded_codepoint_t decode_utf8_codepoint(const char *text)
{
    const unsigned char *bytes = (const unsigned char *)text;

    if (bytes[0] <= 0x7F) {
        return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = true};
    }

    if ((bytes[0] & 0xE0) == 0xC0) {
        if (!is_utf8_continuation(bytes[1])) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        uint32_t codepoint = ((uint32_t)(bytes[0] & 0x1F) << 6) | (uint32_t)(bytes[1] & 0x3F);
        if (codepoint < 0x80) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        return (decoded_codepoint_t){.codepoint = codepoint, .advance = 2, .valid = true};
    }

    if ((bytes[0] & 0xF0) == 0xE0) {
        if (!is_utf8_continuation(bytes[1]) || !is_utf8_continuation(bytes[2])) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        uint32_t codepoint = ((uint32_t)(bytes[0] & 0x0F) << 12) |
                             ((uint32_t)(bytes[1] & 0x3F) << 6) |
                             (uint32_t)(bytes[2] & 0x3F);
        if (codepoint < 0x800 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        return (decoded_codepoint_t){.codepoint = codepoint, .advance = 3, .valid = true};
    }

    if ((bytes[0] & 0xF8) == 0xF0) {
        if (!is_utf8_continuation(bytes[1]) || !is_utf8_continuation(bytes[2]) || !is_utf8_continuation(bytes[3])) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        uint32_t codepoint = ((uint32_t)(bytes[0] & 0x07) << 18) |
                             ((uint32_t)(bytes[1] & 0x3F) << 12) |
                             ((uint32_t)(bytes[2] & 0x3F) << 6) |
                             (uint32_t)(bytes[3] & 0x3F);
        if (codepoint < 0x10000 || codepoint > 0x10FFFF) {
            return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
        }
        return (decoded_codepoint_t){.codepoint = codepoint, .advance = 4, .valid = true};
    }

    return (decoded_codepoint_t){.codepoint = bytes[0], .advance = 1, .valid = false};
}

static int decoded_render_width(decoded_codepoint_t decoded)
{
    if (!decoded.valid || decoded.codepoint <= 0x7F) {
        return ASCII_FONT_WIDTH;
    }
    return ZH_FONT_WIDTH;
}

static int decoded_advance_width(decoded_codepoint_t decoded)
{
    if (!decoded.valid || decoded.codepoint <= 0x7F) {
        return ASCII_FONT_WIDTH + ASCII_FONT_SPACING;
    }
    return ZH_FONT_WIDTH + ZH_FONT_SPACING;
}

static int text_advance_width(const char *text)
{
    const char *value = value_or_empty(text);
    int width = 0;

    for (const char *cursor = value; *cursor != '\0';) {
        decoded_codepoint_t decoded = decode_utf8_codepoint(cursor);
        width += decoded_advance_width(decoded);
        cursor += decoded.advance;
    }

    return width;
}

static void truncate_text_for_width(const char *input, char *output, size_t output_size, int pixel_width)
{
    const char *value = value_or_empty(input);

    if (output_size == 0) {
        return;
    }
    output[0] = '\0';

    if (pixel_width < ASCII_FONT_WIDTH) {
        return;
    }

    const int ellipsis_width = 3 * (ASCII_FONT_WIDTH + ASCII_FONT_SPACING);
    size_t out_len = 0;
    int width = 0;
    const char *cursor = value;
    bool truncated = false;

    while (*cursor != '\0') {
        decoded_codepoint_t decoded = decode_utf8_codepoint(cursor);
        int advance_width = decoded_advance_width(decoded);
        int render_width = decoded_render_width(decoded);
        bool has_more = cursor[decoded.advance] != '\0';
        int reserved_width = has_more ? ellipsis_width : 0;

        if (width + render_width + reserved_width > pixel_width || out_len + decoded.advance >= output_size) {
            truncated = true;
            break;
        }

        memcpy(output + out_len, cursor, decoded.advance);
        out_len += decoded.advance;
        width += advance_width;
        cursor += decoded.advance;
    }

    output[out_len] = '\0';

    if (truncated && output_size > out_len + 1) {
        while (out_len > 0 && width + ellipsis_width > pixel_width) {
            unsigned char byte = (unsigned char)output[out_len - 1];
            out_len--;
            if ((byte & 0xC0) != 0x80) {
                width -= ASCII_FONT_WIDTH + ASCII_FONT_SPACING;
            }
        }
        size_t remaining = output_size - out_len;
        if (remaining >= 4) {
            memcpy(output + out_len, "...", 4);
        } else {
            output[out_len] = '\0';
        }
    }
}

static void format_truncated_line(char *output, size_t output_size, const char *label, const char *value, int pixel_width)
{
    const char *safe_label = value_or_empty(label);
    int label_width = text_advance_width(safe_label);

    if (output_size == 0) {
        return;
    }
    output[0] = '\0';

    if (label_width >= pixel_width) {
        truncate_text_for_width(safe_label, output, output_size, pixel_width);
        return;
    }

    char truncated_value[64];
    int value_width = pixel_width - label_width;
    truncate_text_for_width(value, truncated_value, sizeof(truncated_value), value_width);
    snprintf(output, output_size, "%s%s", safe_label, truncated_value);
}

void display_render_clear(uint8_t *framebuffer, size_t framebuffer_len, bool white)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }

    memset(framebuffer, white ? 0x55 : 0x00, DISPLAY_RENDER_FB_BYTES);
}

void display_render_clear_region(uint8_t *framebuffer, size_t framebuffer_len,
                                 int x, int y, int width, int height, bool white)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len) || width <= 0 || height <= 0) {
        return;
    }

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            display_render_draw_pixel(framebuffer, framebuffer_len, x + col, y + row, !white);
        }
    }
}

void display_render_draw_pixel(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, bool black)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }
    if (x < 0 || y < 0 || x >= DISPLAY_RENDER_WIDTH || y >= DISPLAY_RENDER_HEIGHT) {
        return;
    }

    /* Apply 90° rotation: logical landscape (296×128) → physical portrait (128×296).
     * Logical (x, y) maps to physical X = 127 - y, physical Y = x.
     * This matches the Waveshare Paint_NewImage(buf, 128, 296, 90, WHITE) convention. */
    int phys_x = DISPLAY_RENDER_PHYS_W - 1 - y;
    int phys_y = x;

    size_t index = ((size_t)phys_y * DISPLAY_RENDER_PHYS_W + (size_t)phys_x) / 4;
    int shift = 6 - ((phys_x % 4) * 2);
    uint8_t color = black ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;

    framebuffer[index] &= (uint8_t)~(0x03 << shift);
    framebuffer[index] |= (uint8_t)(color << shift);
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

static void draw_ascii_glyph(uint8_t *framebuffer, size_t framebuffer_len, int x, int y,
                             const uint8_t glyph[ASCII_FONT_WIDTH], bool black)
{
    for (int col = 0; col < ASCII_FONT_WIDTH; col++) {
        for (int row = 0; row < ASCII_FONT_HEIGHT; row++) {
            if ((glyph[col] & (1 << row)) != 0) {
                display_render_draw_pixel(framebuffer, framebuffer_len, x + col, y + row, black);
            }
        }
    }
}

static void draw_enhanced_zh_pixel(uint8_t *framebuffer, size_t framebuffer_len,
                                   int x, int y, int col, int row, bool black)
{
    display_render_draw_pixel(framebuffer, framebuffer_len, x + col, y + row, black);

    if (col + 1 < ZH_FONT_WIDTH) {
        display_render_draw_pixel(framebuffer, framebuffer_len, x + col + 1, y + row, black);
    }
}

static void draw_zh12_glyph(uint8_t *framebuffer, size_t framebuffer_len, int x, int y,
                            const display_zh12_glyph_t *glyph, bool black)
{
    for (int row = 0; row < ZH_FONT_HEIGHT; row++) {
        uint16_t row_bits = ((uint16_t)glyph->bitmap[row * 2] << 8) | glyph->bitmap[row * 2 + 1];
        for (int col = 0; col < ZH_FONT_WIDTH; col++) {
            if ((row_bits & (uint16_t)(0x8000 >> col)) != 0) {
                draw_enhanced_zh_pixel(framebuffer, framebuffer_len, x, y, col, row, black);
            }
        }
    }
}

static void draw_unknown_zh_box(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, bool black)
{
    display_render_draw_rect(framebuffer, framebuffer_len, x, y, ZH_FONT_WIDTH, ZH_FONT_HEIGHT, black);
    display_render_draw_hline(framebuffer, framebuffer_len, x + 2, y + ZH_FONT_HEIGHT / 2, ZH_FONT_WIDTH - 4, black);
}

int display_render_draw_text(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, const char *text, bool black)
{
    if (!text || !framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return x;
    }

    int cursor_x = x;
    const char *cursor = text;
    while (*cursor != '\0') {
        decoded_codepoint_t decoded = decode_utf8_codepoint(cursor);

        if (!decoded.valid || decoded.codepoint <= 0x7F) {
            if (cursor_x > DISPLAY_RENDER_WIDTH - ASCII_FONT_WIDTH) {
                break;
            }
            uint8_t glyph[ASCII_FONT_WIDTH];
            glyph_for_char((char)decoded.codepoint, glyph);
            draw_ascii_glyph(framebuffer, framebuffer_len, cursor_x, y, glyph, black);
            cursor_x += ASCII_FONT_WIDTH + ASCII_FONT_SPACING;
            cursor += decoded.advance;
            continue;
        }

        if (cursor_x > DISPLAY_RENDER_WIDTH - ZH_FONT_WIDTH) {
            break;
        }

        const display_zh12_glyph_t *glyph = display_font_zh12_find(decoded.codepoint);
        if (glyph) {
            draw_zh12_glyph(framebuffer, framebuffer_len, cursor_x, y, glyph, black);
        } else {
            draw_unknown_zh_box(framebuffer, framebuffer_len, cursor_x, y, black);
        }
        cursor_x += ZH_FONT_WIDTH + ZH_FONT_SPACING;
        cursor += decoded.advance;
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

static void render_header_region(uint8_t *framebuffer, size_t framebuffer_len,
                                 const display_dashboard_data_t *data)
{
    display_render_clear_region(framebuffer, framebuffer_len,
                                HEADER_REGION_X, HEADER_REGION_Y,
                                HEADER_REGION_WIDTH, HEADER_REGION_HEIGHT, true);

    const char *date = (data && data->date && data->date[0] != '\0') ? data->date : "";
    const char *weekday = (data && data->weekday && data->weekday[0] != '\0') ? data->weekday : "";
    const char *time = (data && data->time && data->time[0] != '\0') ? data->time : "Time syncing...";
    char header_raw[80];
    char header[80];
    if (date[0] != '\0' && weekday[0] != '\0') {
        snprintf(header_raw, sizeof(header_raw), "%s %s  %s", date, weekday, time);
    } else if (date[0] != '\0') {
        snprintf(header_raw, sizeof(header_raw), "%s  %s", date, time);
    } else {
        snprintf(header_raw, sizeof(header_raw), "%s", time);
    }
    truncate_text_for_width(header_raw, header, sizeof(header), HEADER_TEXT_WIDTH);
    display_render_draw_text(framebuffer, framebuffer_len, HEADER_TEXT_X, 7, header, true);
}

static void render_weather_region(uint8_t *framebuffer, size_t framebuffer_len,
                                  const display_dashboard_data_t *data)
{
    display_render_clear_region(framebuffer, framebuffer_len,
                                WEATHER_REGION_X, WEATHER_REGION_Y,
                                WEATHER_REGION_WIDTH, WEATHER_REGION_HEIGHT, true);
    bool has_weather = data && data->weather_city && data->weather_city[0] != '\0';
    draw_text_line(framebuffer, framebuffer_len, WEATHER_TEXT_X, 31, WEATHER_TEXT_WIDTH,
                   "Weather: ", has_weather ? data->weather_city : "set city in chat");
    if (has_weather && data->weather_summary && data->weather_summary[0] != '\0') {
        draw_text_line(framebuffer, framebuffer_len, WEATHER_TEXT_X, 31 + LINE_HEIGHT, WEATHER_TEXT_WIDTH,
                       "", data->weather_summary);
    }
}

static void render_todos_region(uint8_t *framebuffer, size_t framebuffer_len,
                                const display_dashboard_data_t *data)
{
    display_render_clear_region(framebuffer, framebuffer_len,
                                TODOS_REGION_X, TODOS_REGION_Y,
                                TODOS_REGION_WIDTH, TODOS_REGION_HEIGHT, true);
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

void display_render_dashboard_region(uint8_t *framebuffer, size_t framebuffer_len,
                                     const display_dashboard_data_t *data,
                                     display_render_region_t region)
{
    if (!framebuffer_is_valid(framebuffer, framebuffer_len)) {
        return;
    }

    switch (region) {
    case DISPLAY_RENDER_REGION_HEADER:
        render_header_region(framebuffer, framebuffer_len, data);
        break;
    case DISPLAY_RENDER_REGION_WEATHER:
        render_weather_region(framebuffer, framebuffer_len, data);
        break;
    case DISPLAY_RENDER_REGION_TODOS:
        render_todos_region(framebuffer, framebuffer_len, data);
        break;
    case DISPLAY_RENDER_REGION_FULL:
    default:
        display_render_dashboard(framebuffer, framebuffer_len, data);
        break;
    }
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
    render_header_region(framebuffer, framebuffer_len, data);
    render_weather_region(framebuffer, framebuffer_len, data);
    render_todos_region(framebuffer, framebuffer_len, data);
}
