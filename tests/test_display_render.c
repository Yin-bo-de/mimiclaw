#include "display/display_render.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool framebuffer_has_black_pixel_at(const uint8_t *framebuffer, int x, int y)
{
    int phys_x = DISPLAY_RENDER_PHYS_W - 1 - y;
    int phys_y = x;
    size_t index = ((size_t)phys_y * DISPLAY_RENDER_PHYS_W + (size_t)phys_x) / 4;
    int shift = 6 - ((phys_x % 4) * 2);
    return ((framebuffer[index] >> shift) & 0x03) == 0x00;
}

static bool framebuffer_region_has_black_pixel(const uint8_t *framebuffer, int x, int y, int width, int height)
{
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            if (framebuffer_has_black_pixel_at(framebuffer, x + col, y + row)) {
                return true;
            }
        }
    }
    return false;
}

static void chinese_weather_uses_real_glyphs_not_unknown_boxes(void)
{
    uint8_t unknown_framebuffer[DISPLAY_RENDER_FB_BYTES];
    uint8_t chinese_framebuffer[DISPLAY_RENDER_FB_BYTES];

    display_render_clear(unknown_framebuffer, sizeof(unknown_framebuffer), true);
    display_render_clear(chinese_framebuffer, sizeof(chinese_framebuffer), true);

    display_render_draw_text(unknown_framebuffer, sizeof(unknown_framebuffer), 0, 0, "\x80\x80\x80\x80\x80\x80", true);
    display_render_draw_text(chinese_framebuffer, sizeof(chinese_framebuffer), 0, 0, "多云", true);

    assert(memcmp(unknown_framebuffer, chinese_framebuffer, sizeof(chinese_framebuffer)) != 0);
    assert(framebuffer_region_has_black_pixel(chinese_framebuffer, 0, 0, 24, 12));
}

static void chinese_text_advances_by_12px_glyphs(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];

    display_render_clear(framebuffer, sizeof(framebuffer), true);

    int cursor_x = display_render_draw_text(framebuffer, sizeof(framebuffer), 0, 0, "多云", true);

    assert(cursor_x == 26);
}

static void mixed_ascii_and_chinese_render_together(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];

    display_render_clear(framebuffer, sizeof(framebuffer), true);

    int cursor_x = display_render_draw_text(framebuffer, sizeof(framebuffer), 0, 0, "北京 25C", true);

    assert(cursor_x > 0);
    assert(framebuffer_region_has_black_pixel(framebuffer, 0, 0, 24, 12));
    assert(framebuffer_region_has_black_pixel(framebuffer, 26, 0, 40, 12));
}

static void invalid_utf8_still_renders_one_unknown_box_per_byte(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];

    display_render_clear(framebuffer, sizeof(framebuffer), true);

    int cursor_x = display_render_draw_text(framebuffer, sizeof(framebuffer), 0, 0, "\x80\x80", true);

    assert(cursor_x == 12);
}

static void unsupported_valid_unicode_uses_wide_placeholder(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];

    display_render_clear(framebuffer, sizeof(framebuffer), true);

    int cursor_x = display_render_draw_text(framebuffer, sizeof(framebuffer), 0, 0, "龘", true);

    assert(cursor_x == 13);
    assert(framebuffer_region_has_black_pixel(framebuffer, 0, 0, 12, 12));
}

int main(void)
{
    chinese_weather_uses_real_glyphs_not_unknown_boxes();
    chinese_text_advances_by_12px_glyphs();
    mixed_ascii_and_chinese_render_together();
    invalid_utf8_still_renders_one_unknown_box_per_byte();
    unsupported_valid_unicode_uses_wide_placeholder();
    return 0;
}
