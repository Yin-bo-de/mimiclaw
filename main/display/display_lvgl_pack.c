#include "display/display_lvgl_pack.h"

display_lvgl_epaper_color_t display_lvgl_rgb565_to_epaper(uint16_t rgb565)
{
    uint8_t red = (uint8_t)(((rgb565 >> 11) & 0x1F) * 255 / 31);
    uint8_t green = (uint8_t)(((rgb565 >> 5) & 0x3F) * 255 / 63);
    uint8_t blue = (uint8_t)((rgb565 & 0x1F) * 255 / 31);

    if (red > 180 && green < 100 && blue < 100) {
        return DISPLAY_LVGL_EPAPER_RED;
    }
    if (red > 180 && green > 140 && blue < 120) {
        return DISPLAY_LVGL_EPAPER_YELLOW;
    }

    int luminance = (red * 299 + green * 587 + blue * 114) / 1000;
    return luminance < MIMI_DISPLAY_LVGL_BLACK_THRESHOLD ? DISPLAY_LVGL_EPAPER_BLACK : DISPLAY_LVGL_EPAPER_WHITE;
}

bool display_lvgl_pack_epaper_pixel(uint8_t *framebuffer, size_t framebuffer_len,
                                    int logical_x, int logical_y,
                                    display_lvgl_epaper_color_t color)
{
    if (!framebuffer || framebuffer_len < MIMI_DISPLAY_FB_BYTES) {
        return false;
    }
    if (logical_x < 0 || logical_x >= MIMI_DISPLAY_ROTATED_WIDTH ||
        logical_y < 0 || logical_y >= MIMI_DISPLAY_ROTATED_HEIGHT) {
        return false;
    }
    if (color < DISPLAY_LVGL_EPAPER_BLACK || color > DISPLAY_LVGL_EPAPER_RED) {
        return false;
    }

    int phys_x = MIMI_DISPLAY_WIDTH - 1 - logical_y;
    int phys_y = logical_x;
    size_t index = ((size_t)phys_y * MIMI_DISPLAY_WIDTH + (size_t)phys_x) / 4;
    int shift = 6 - ((phys_x % 4) * 2);

    framebuffer[index] = (uint8_t)((framebuffer[index] & ~(0x03u << shift)) |
                                  (((uint8_t)color & 0x03u) << shift));
    return true;
}
