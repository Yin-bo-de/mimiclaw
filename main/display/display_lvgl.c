#include "display/display_lvgl.h"
#include "display/display_font_zh_gb2312_16.h"
#include "display/display_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "display_lvgl";

static uint8_t *s_framebuffer;
static size_t s_framebuffer_len;
static lv_display_t *s_display;
static void *s_draw_buffer;
static lv_obj_t *s_root;
static lv_obj_t *s_header_label;
static lv_obj_t *s_weather_city_label;
static lv_obj_t *s_weather_summary_label;
static lv_obj_t *s_todos_title_label;
static lv_obj_t *s_todo_labels[MIMI_DISPLAY_MAX_TODOS];
static lv_obj_t *s_quote_label;
static lv_obj_t *s_header_separator;
static lv_obj_t *s_column_separator;
static lv_obj_t *s_quote_separator;

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    if (!s_framebuffer || s_framebuffer_len < MIMI_DISPLAY_FB_BYTES) {
        lv_display_flush_ready(display);
        return;
    }

    int width = lv_area_get_width(area);
    const uint16_t *colors = (const uint16_t *)px_map;

    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint16_t color = colors[(y - area->y1) * width + (x - area->x1)];
            display_lvgl_pack_epaper_pixel(s_framebuffer, s_framebuffer_len, x, y,
                                           display_lvgl_rgb565_to_epaper(color));
        }
    }

    lv_display_flush_ready(display);
}

static void configure_label(lv_obj_t *label, int x, int y, int width, int height)
{
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_font(label, &mimi_font_zh_gb2312_16, 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

static lv_obj_t *create_separator(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *separator = lv_obj_create(parent);
    lv_obj_set_pos(separator, x, y);
    lv_obj_set_size(separator, width, height);
    lv_obj_set_style_bg_color(separator, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(separator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(separator, 0, 0);
    lv_obj_set_style_pad_all(separator, 0, 0);
    lv_obj_clear_flag(separator, LV_OBJ_FLAG_SCROLLABLE);
    return separator;
}

static void create_dashboard_objects(void)
{
    s_root = lv_obj_create(NULL);
    lv_obj_set_size(s_root, DISPLAY_LVGL_WIDTH, DISPLAY_LVGL_HEIGHT);
    lv_obj_set_style_bg_color(s_root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 1, 0);
    lv_obj_set_style_border_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_border_side(s_root, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_header_separator = create_separator(s_root, 0, 21, DISPLAY_LVGL_WIDTH, 1);
    s_column_separator = create_separator(s_root, 156, 22, 1, 88);
    s_quote_separator = create_separator(s_root, 0, 110, DISPLAY_LVGL_WIDTH, 1);

    s_header_label = lv_label_create(s_root);
    configure_label(s_header_label, 4, 2, 288, 18);

    s_weather_city_label = lv_label_create(s_root);
    configure_label(s_weather_city_label, 6, 24, 140, 18);
    lv_obj_set_style_text_color(s_weather_city_label, lv_color_hex(0xF5C400), 0);

    s_weather_summary_label = lv_label_create(s_root);
    configure_label(s_weather_summary_label, 6, 44, 140, 44);
    lv_obj_set_style_text_color(s_weather_summary_label, lv_color_hex(0xF5C400), 0);
    lv_label_set_long_mode(s_weather_summary_label, LV_LABEL_LONG_WRAP);

    s_todos_title_label = lv_label_create(s_root);
    configure_label(s_todos_title_label, 158, 24, 130, 18);

    for (size_t i = 0; i < MIMI_DISPLAY_MAX_TODOS; i++) {
        s_todo_labels[i] = lv_label_create(s_root);
        configure_label(s_todo_labels[i], 158, 44 + (int)i * 13, 124, 14);
    }

    s_quote_label = lv_label_create(s_root);
    configure_label(s_quote_label, 6, 111, 284, 16);
    lv_obj_set_style_text_align(s_quote_label, LV_TEXT_ALIGN_CENTER, 0);

    lv_screen_load(s_root);
}

esp_err_t display_lvgl_init(uint8_t *framebuffer, size_t framebuffer_len)
{
    if (!framebuffer || framebuffer_len < MIMI_DISPLAY_FB_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_display) {
        return ESP_OK;
    }

    s_framebuffer = framebuffer;
    s_framebuffer_len = framebuffer_len;
    memset(s_framebuffer, 0x55, MIMI_DISPLAY_FB_BYTES);

    lv_init();
    s_display = lv_display_create(DISPLAY_LVGL_WIDTH, DISPLAY_LVGL_HEIGHT);
    if (!s_display) {
        return ESP_ERR_NO_MEM;
    }

    size_t draw_buffer_size = DISPLAY_LVGL_WIDTH * DISPLAY_LVGL_HEIGHT *
                              lv_color_format_get_size(lv_display_get_color_format(s_display));
    s_draw_buffer = heap_caps_malloc(draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_draw_buffer) {
        s_draw_buffer = heap_caps_malloc(draw_buffer_size, MALLOC_CAP_8BIT);
    }
    if (!s_draw_buffer) {
        ESP_LOGE(TAG, "failed to allocate LVGL draw buffer (%u bytes)", (unsigned)draw_buffer_size);
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(s_display, s_draw_buffer, NULL, draw_buffer_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_display, flush_cb);
    create_dashboard_objects();

    ESP_LOGI(TAG, "LVGL display initialized");
    return ESP_OK;
}

esp_err_t display_lvgl_render_dashboard(const display_dashboard_data_t *data)
{
    if (!data || !s_display || !s_framebuffer || s_framebuffer_len < MIMI_DISPLAY_FB_BYTES) {
        return ESP_ERR_INVALID_STATE;
    }

    char header[64];
    snprintf(header, sizeof(header), "%s %s %s",
             data->date ? data->date : "",
             data->weekday ? data->weekday : "",
             data->time ? data->time : "");

    memset(s_framebuffer, 0x55, MIMI_DISPLAY_FB_BYTES);
    lv_label_set_text(s_header_label, header);
    lv_label_set_text(s_weather_city_label, data->weather_city && data->weather_city[0] ? data->weather_city : "当前位置");
    lv_label_set_text(s_weather_summary_label, data->weather_summary && data->weather_summary[0] ? data->weather_summary : "天气待更新");
    lv_label_set_text(s_todos_title_label, "待办");

    for (size_t i = 0; i < MIMI_DISPLAY_MAX_TODOS; i++) {
        if (i < data->todo_count && data->todos[i] && data->todos[i][0]) {
            char line[MIMI_DISPLAY_TODO_LEN + 4];
            snprintf(line, sizeof(line), "- %s", data->todos[i]);
            lv_label_set_text(s_todo_labels[i], line);
        } else {
            lv_label_set_text(s_todo_labels[i], "");
        }
    }

    lv_label_set_text(s_quote_label, data->quote && data->quote[0] ? data->quote : "");

    lv_obj_invalidate(s_root);
    lv_refr_now(s_display);
    return ESP_OK;
}
