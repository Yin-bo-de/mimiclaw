#include "display/display_lvgl_pack.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint8_t get_physical_color(const uint8_t *framebuffer, int phys_x, int phys_y)
{
    size_t index = ((size_t)phys_y * MIMI_DISPLAY_WIDTH + (size_t)phys_x) / 4;
    int shift = 6 - ((phys_x % 4) * 2);
    return (framebuffer[index] >> shift) & 0x03;
}

static void pack_origin_rotates_to_top_right(void)
{
    uint8_t framebuffer[MIMI_DISPLAY_FB_BYTES];
    memset(framebuffer, 0x55, sizeof(framebuffer));

    assert(display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), 0, 0, DISPLAY_LVGL_EPAPER_BLACK));

    assert(get_physical_color(framebuffer, MIMI_DISPLAY_WIDTH - 1, 0) == DISPLAY_LVGL_EPAPER_BLACK);
}

static void pack_bottom_right_rotates_to_bottom_left(void)
{
    uint8_t framebuffer[MIMI_DISPLAY_FB_BYTES];
    memset(framebuffer, 0x55, sizeof(framebuffer));

    assert(display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer),
                                                   MIMI_DISPLAY_ROTATED_WIDTH - 1,
                                                   MIMI_DISPLAY_ROTATED_HEIGHT - 1,
                                                   DISPLAY_LVGL_EPAPER_RED));

    assert(get_physical_color(framebuffer, 0, MIMI_DISPLAY_HEIGHT - 1) == DISPLAY_LVGL_EPAPER_RED);
}

static void pack_rejects_out_of_bounds_coordinates(void)
{
    uint8_t framebuffer[MIMI_DISPLAY_FB_BYTES];
    uint8_t before[MIMI_DISPLAY_FB_BYTES];
    memset(framebuffer, 0x55, sizeof(framebuffer));
    memcpy(before, framebuffer, sizeof(before));

    assert(!display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), -1, 0, DISPLAY_LVGL_EPAPER_BLACK));
    assert(!display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), 0, -1, DISPLAY_LVGL_EPAPER_BLACK));
    assert(!display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), MIMI_DISPLAY_ROTATED_WIDTH, 0, DISPLAY_LVGL_EPAPER_BLACK));
    assert(!display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), 0, MIMI_DISPLAY_ROTATED_HEIGHT, DISPLAY_LVGL_EPAPER_BLACK));

    assert(memcmp(framebuffer, before, sizeof(framebuffer)) == 0);
}

static void pack_rejects_short_framebuffer(void)
{
    uint8_t framebuffer[MIMI_DISPLAY_FB_BYTES - 1];
    memset(framebuffer, 0x55, sizeof(framebuffer));

    assert(!display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), 0, 0, DISPLAY_LVGL_EPAPER_BLACK));
}

static void pack_does_not_modify_extra_tail_bytes(void)
{
    uint8_t framebuffer[MIMI_DISPLAY_FB_BYTES + 4];
    memset(framebuffer, 0x55, MIMI_DISPLAY_FB_BYTES);
    memset(framebuffer + MIMI_DISPLAY_FB_BYTES, 0xA5, 4);

    assert(display_lvgl_pack_epaper_pixel(framebuffer, sizeof(framebuffer), 0, 0, DISPLAY_LVGL_EPAPER_BLACK));

    assert(framebuffer[MIMI_DISPLAY_FB_BYTES] == 0xA5);
    assert(framebuffer[MIMI_DISPLAY_FB_BYTES + 1] == 0xA5);
    assert(framebuffer[MIMI_DISPLAY_FB_BYTES + 2] == 0xA5);
    assert(framebuffer[MIMI_DISPLAY_FB_BYTES + 3] == 0xA5);
}

static void rgb565_maps_to_epaper_colors(void)
{
    assert(display_lvgl_rgb565_to_epaper(0x0000) == DISPLAY_LVGL_EPAPER_BLACK);
    assert(display_lvgl_rgb565_to_epaper(0xFFFF) == DISPLAY_LVGL_EPAPER_WHITE);
    assert(display_lvgl_rgb565_to_epaper(0xF800) == DISPLAY_LVGL_EPAPER_RED);
    assert(display_lvgl_rgb565_to_epaper(0xFFE0) == DISPLAY_LVGL_EPAPER_YELLOW);
}

int main(void)
{
    pack_origin_rotates_to_top_right();
    pack_bottom_right_rotates_to_bottom_left();
    pack_rejects_out_of_bounds_coordinates();
    pack_rejects_short_framebuffer();
    pack_does_not_modify_extra_tail_bytes();
    rgb565_maps_to_epaper_colors();
    return 0;
}
