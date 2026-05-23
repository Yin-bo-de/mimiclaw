#pragma once

#include "mimi_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Logical (rotated 90°) drawing dimensions — dashboard renders in landscape */
#define DISPLAY_RENDER_WIDTH      MIMI_DISPLAY_ROTATED_WIDTH
#define DISPLAY_RENDER_HEIGHT     MIMI_DISPLAY_ROTATED_HEIGHT
/* Physical framebuffer size (portrait 128×296 at 2bpp) */
#define DISPLAY_RENDER_FB_BYTES   MIMI_DISPLAY_FB_BYTES
/* Physical resolution for framebuffer address calculation */
#define DISPLAY_RENDER_PHYS_W     MIMI_DISPLAY_WIDTH
#define DISPLAY_RENDER_PHYS_H     MIMI_DISPLAY_HEIGHT

typedef struct {
    const char *date;
    const char *weekday;
    const char *time;
    const char *weather_city;
    const char *weather_summary;
    const char *todos[5];
    size_t todo_count;
} display_dashboard_data_t;

typedef enum {
    DISPLAY_RENDER_REGION_FULL = 0,
    DISPLAY_RENDER_REGION_HEADER,
    DISPLAY_RENDER_REGION_WEATHER,
    DISPLAY_RENDER_REGION_TODOS,
} display_render_region_t;

void display_render_clear(uint8_t *framebuffer, size_t framebuffer_len, bool white);
void display_render_clear_region(uint8_t *framebuffer, size_t framebuffer_len,
                                 int x, int y, int width, int height, bool white);
void display_render_draw_pixel(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, bool black);
void display_render_draw_hline(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int width, bool black);
void display_render_draw_vline(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int height, bool black);
void display_render_draw_rect(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, int width, int height, bool black);
int display_render_draw_text(uint8_t *framebuffer, size_t framebuffer_len, int x, int y, const char *text, bool black);
void display_render_dashboard(uint8_t *framebuffer, size_t framebuffer_len, const display_dashboard_data_t *data);
void display_render_dashboard_region(uint8_t *framebuffer, size_t framebuffer_len,
                                     const display_dashboard_data_t *data,
                                     display_render_region_t region);

#ifdef __cplusplus
}
#endif
