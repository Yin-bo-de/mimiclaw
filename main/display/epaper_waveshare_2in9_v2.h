#pragma once

#include "esp_err.h"
#include "mimi_config.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EPAPER_2IN9_V2_WIDTH      MIMI_DISPLAY_WIDTH
#define EPAPER_2IN9_V2_HEIGHT     MIMI_DISPLAY_HEIGHT
#define EPAPER_2IN9_V2_FB_BYTES   MIMI_DISPLAY_FB_BYTES

typedef struct {
    int mosi_pin;
    int sclk_pin;
    int cs_pin;
    int dc_pin;
    int rst_pin;
    int busy_pin;
    int spi_clock_hz;
    int busy_timeout_ms;
} epaper_waveshare_2in9_v2_config_t;

esp_err_t epaper_waveshare_2in9_v2_init(void);
esp_err_t epaper_waveshare_2in9_v2_init_with_config(const epaper_waveshare_2in9_v2_config_t *config);
esp_err_t epaper_waveshare_2in9_v2_display_frame(const uint8_t *framebuffer, size_t framebuffer_len);
esp_err_t epaper_waveshare_2in9_v2_sleep(void);
esp_err_t epaper_waveshare_2in9_v2_test_pattern(void);

#ifdef __cplusplus
}
#endif
