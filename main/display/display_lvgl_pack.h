#pragma once

#include "mimi_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DISPLAY_LVGL_EPAPER_BLACK = 0x00,
    DISPLAY_LVGL_EPAPER_WHITE = 0x01,
    DISPLAY_LVGL_EPAPER_YELLOW = 0x02,
    DISPLAY_LVGL_EPAPER_RED = 0x03,
} display_lvgl_epaper_color_t;

bool display_lvgl_pack_epaper_pixel(uint8_t *framebuffer, size_t framebuffer_len,
                                    int logical_x, int logical_y,
                                    display_lvgl_epaper_color_t color);
display_lvgl_epaper_color_t display_lvgl_rgb565_to_epaper(uint16_t rgb565);

#ifdef __cplusplus
}
#endif
