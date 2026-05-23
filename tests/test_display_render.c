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

static void dashboard_header_renders_weekday_between_date_and_time(void)
{
    uint8_t with_weekday[DISPLAY_RENDER_FB_BYTES];
    uint8_t without_weekday[DISPLAY_RENDER_FB_BYTES];

    display_dashboard_data_t data = {
        .date = "2026-05-23",
        .weekday = "周六",
        .time = "14:30",
    };
    display_dashboard_data_t baseline = {
        .date = "2026-05-23",
        .time = "14:30",
    };

    display_render_dashboard(with_weekday, sizeof(with_weekday), &data);
    display_render_dashboard(without_weekday, sizeof(without_weekday), &baseline);

    assert(memcmp(with_weekday, without_weekday, sizeof(with_weekday)) != 0);
    assert(framebuffer_region_has_black_pixel(with_weekday, 73, 7, 25, 12));
}

static void chinese_glyphs_are_horizontally_enhanced(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];
    int black_pixels = 0;

    display_render_clear(framebuffer, sizeof(framebuffer), true);
    display_render_draw_text(framebuffer, sizeof(framebuffer), 0, 0, "周", true);

    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < 12; x++) {
            if (framebuffer_has_black_pixel_at(framebuffer, x, y)) {
                black_pixels++;
            }
        }
    }

    assert(black_pixels > 41);
}

static void dashboard_region_render_updates_only_requested_area(void)
{
    uint8_t framebuffer[DISPLAY_RENDER_FB_BYTES];
    uint8_t before[DISPLAY_RENDER_FB_BYTES];

    display_dashboard_data_t baseline = {
        .date = "2026-05-23",
        .weekday = "周六",
        .time = "14:30",
        .weather_city = "北京",
        .weather_summary = "晴",
    };
    display_dashboard_data_t changed = baseline;
    changed.weather_city = "上海";
    changed.weather_summary = "多云";

    display_render_dashboard(framebuffer, sizeof(framebuffer), &baseline);
    memcpy(before, framebuffer, sizeof(before));

    display_render_dashboard_region(framebuffer, sizeof(framebuffer), &changed, DISPLAY_RENDER_REGION_WEATHER);

    assert(memcmp(framebuffer, before, sizeof(framebuffer)) != 0);
    assert(framebuffer_region_has_black_pixel(framebuffer, 8, 31, 145, 28));
    for (int y = 1; y < 21; y++) {
        for (int x = 1; x < 295; x++) {
            assert(framebuffer_has_black_pixel_at(framebuffer, x, y) == framebuffer_has_black_pixel_at(before, x, y));
        }
    }
}

int main(void)
{
    chinese_weather_uses_real_glyphs_not_unknown_boxes();
    chinese_text_advances_by_12px_glyphs();
    mixed_ascii_and_chinese_render_together();
    invalid_utf8_still_renders_one_unknown_box_per_byte();
    unsupported_valid_unicode_uses_wide_placeholder();
    dashboard_header_renders_weekday_between_date_and_time();
    chinese_glyphs_are_horizontally_enhanced();
    dashboard_region_render_updates_only_requested_area();
    return 0;
}
