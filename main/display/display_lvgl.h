#pragma once

#include "display/display_lvgl_pack.h"

#include "esp_err.h"
#include "mimi_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_LVGL_WIDTH    MIMI_DISPLAY_ROTATED_WIDTH
#define DISPLAY_LVGL_HEIGHT   MIMI_DISPLAY_ROTATED_HEIGHT
#define DISPLAY_LVGL_FB_BYTES MIMI_DISPLAY_FB_BYTES

typedef struct {
    const char *date;
    const char *weekday;
    const char *time;
    const char *weather_city;
    const char *weather_summary;
    const char *todos[5];
    size_t todo_count;
    const char *quote;
} display_dashboard_data_t;

esp_err_t display_lvgl_init(uint8_t *framebuffer, size_t framebuffer_len);
esp_err_t display_lvgl_render_dashboard(const display_dashboard_data_t *data);

#ifdef __cplusplus
}
#endif
