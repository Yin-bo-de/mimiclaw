#include "display/epaper_waveshare_2in9_v2.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "epaper_2in9_g";

#define EPD_G_CMD_PANEL_SETTING              0x00
#define EPD_G_CMD_POWER_SETTING              0x01
#define EPD_G_CMD_POWER_OFF                  0x02
#define EPD_G_CMD_POWER_ON                   0x04
#define EPD_G_CMD_DEEP_SLEEP                 0x07
#define EPD_G_CMD_DATA_START_TRANSMISSION    0x10
#define EPD_G_CMD_DISPLAY_REFRESH            0x12
#define EPD_G_CMD_BOOSTER_SOFT_START         0x06
#define EPD_G_CMD_RESOLUTION_SETTING         0x61

#define EPD_G_COLOR_BLACK                    0x00
#define EPD_G_COLOR_WHITE                    0x01
#define EPD_G_COLOR_YELLOW                   0x02
#define EPD_G_COLOR_RED                      0x03

static spi_device_handle_t s_spi;
static epaper_waveshare_2in9_v2_config_t s_config;
static bool s_initialized;

static const epaper_waveshare_2in9_v2_config_t DEFAULT_CONFIG = {
    .mosi_pin = MIMI_DISPLAY_PIN_MOSI,
    .sclk_pin = MIMI_DISPLAY_PIN_SCLK,
    .cs_pin = MIMI_DISPLAY_PIN_CS,
    .dc_pin = MIMI_DISPLAY_PIN_DC,
    .rst_pin = MIMI_DISPLAY_PIN_RST,
    .busy_pin = MIMI_DISPLAY_PIN_BUSY,
    .pwr_pin = MIMI_DISPLAY_PIN_PWR,
    .spi_clock_hz = MIMI_DISPLAY_SPI_CLOCK_HZ,
    .busy_timeout_ms = MIMI_DISPLAY_BUSY_TIMEOUT_MS,
};

static int elapsed_ms(TickType_t start)
{
    return (int)(pdTICKS_TO_MS(xTaskGetTickCount() - start));
}

static bool epaper_has_pwr_pin(void)
{
    return s_config.pwr_pin >= 0;
}

static int epaper_get_pwr_level(void)
{
    return epaper_has_pwr_pin() ? gpio_get_level(s_config.pwr_pin) : -1;
}

static esp_err_t epaper_set_pwr_level(int level)
{
    return epaper_has_pwr_pin() ? gpio_set_level(s_config.pwr_pin, level) : ESP_OK;
}

static void epaper_log_levels(const char *stage)
{
    ESP_LOGI(TAG, "%s: PWR=%d RST=%d BUSY=%d", stage,
             epaper_get_pwr_level(),
             gpio_get_level(s_config.rst_pin),
             gpio_get_level(s_config.busy_pin));
}

static TickType_t delay_ticks_at_least_one(int delay_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks > 0 ? ticks : 1;
}

static esp_err_t epaper_wait_busy_high(const char *stage)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(s_config.busy_timeout_ms);
    TickType_t poll_delay = delay_ticks_at_least_one(20);
    int initial_level = gpio_get_level(s_config.busy_pin);

    vTaskDelay(delay_ticks_at_least_one(100));
    ESP_LOGI(TAG, "busy-high wait start: %s BUSY=%d", stage, initial_level);
    while (gpio_get_level(s_config.busy_pin) == 0) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "busy-high timeout during %s after %d ms; BUSY=%d RST=%d PWR=%d",
                     stage, s_config.busy_timeout_ms,
                     gpio_get_level(s_config.busy_pin),
                     gpio_get_level(s_config.rst_pin),
                     epaper_get_pwr_level());
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll_delay);
    }

    ESP_LOGI(TAG, "busy-high wait done: %s initial=%d final=%d elapsed=%d ms", stage,
             initial_level, gpio_get_level(s_config.busy_pin), elapsed_ms(start));
    return ESP_OK;
}

static esp_err_t epaper_write_command(uint8_t command)
{
    spi_transaction_t transaction = {
        .length = 8,
        .tx_buffer = &command,
    };

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.dc_pin, 0), TAG, "set DC low failed");
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t epaper_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_transaction_t transaction = {
        .length = len * 8,
        .tx_buffer = data,
    };

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.dc_pin, 1), TAG, "set DC high failed");
    return spi_device_polling_transmit(s_spi, &transaction);
}

static esp_err_t epaper_write_u8(uint8_t data)
{
    return epaper_write_data(&data, 1);
}

static esp_err_t epaper_reset(void)
{
    ESP_RETURN_ON_ERROR(epaper_set_pwr_level(1), TAG, "power high failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    epaper_log_levels("reset before drive");

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(MIMI_DISPLAY_RESET_HIGH_MS));
    epaper_log_levels("reset high");

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 0), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(MIMI_DISPLAY_RESET_LOW_MS));
    epaper_log_levels("reset low");

    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 1), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(MIMI_DISPLAY_RESET_RELEASE_MS));
    epaper_log_levels("reset released");
    return ESP_OK;
}

static esp_err_t epaper_turn_on_display(void)
{
    epaper_log_levels("refresh before command");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_DISPLAY_REFRESH), TAG, "refresh cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "refresh data failed");
    epaper_log_levels("refresh after command");
    return epaper_wait_busy_high("display refresh");
}

static esp_err_t epaper_panel_init(void)
{
    ESP_LOGI(TAG, "initializing Waveshare 2.9in G four-color e-paper");
    ESP_RETURN_ON_ERROR(epaper_reset(), TAG, "hardware reset failed");
    ESP_RETURN_ON_ERROR(epaper_wait_busy_high("post reset"), TAG, "post reset busy failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x4D), TAG, "0x4D cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x78), TAG, "0x4D data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_PANEL_SETTING), TAG, "panel setting cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x0F), TAG, "panel setting data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x29), TAG, "panel setting data 2 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_POWER_SETTING), TAG, "power setting cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x07), TAG, "power setting data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "power setting data 2 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x03), TAG, "0x03 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x10), TAG, "0x03 data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x54), TAG, "0x03 data 2 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x44), TAG, "0x03 data 3 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_BOOSTER_SOFT_START), TAG, "booster cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x0F), TAG, "booster data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x0A), TAG, "booster data 2 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x2F), TAG, "booster data 3 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x25), TAG, "booster data 4 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x22), TAG, "booster data 5 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x2E), TAG, "booster data 6 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x21), TAG, "booster data 7 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x41), TAG, "0x41 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "0x41 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x50), TAG, "0x50 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x37), TAG, "0x50 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x60), TAG, "0x60 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x02), TAG, "0x60 data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x02), TAG, "0x60 data 2 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_RESOLUTION_SETTING), TAG, "resolution cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(EPAPER_2IN9_V2_WIDTH / 256), TAG, "width high failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(EPAPER_2IN9_V2_WIDTH % 256), TAG, "width low failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(EPAPER_2IN9_V2_HEIGHT / 256), TAG, "height high failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(EPAPER_2IN9_V2_HEIGHT % 256), TAG, "height low failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x65), TAG, "0x65 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "0x65 data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "0x65 data 2 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "0x65 data 3 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "0x65 data 4 failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0xE7), TAG, "0xE7 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x1C), TAG, "0xE7 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0xE3), TAG, "0xE3 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x22), TAG, "0xE3 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0xB4), TAG, "0xB4 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0xD0), TAG, "0xB4 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0xB5), TAG, "0xB5 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x03), TAG, "0xB5 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0xE9), TAG, "0xE9 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x01), TAG, "0xE9 data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(0x30), TAG, "0x30 cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x08), TAG, "0x30 data failed");

    epaper_log_levels("power on before command");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_POWER_ON), TAG, "power on cmd failed");
    vTaskDelay(pdMS_TO_TICKS(500));
    epaper_log_levels("power on after command");
    ESP_RETURN_ON_ERROR(epaper_wait_busy_high("power on"), TAG, "power on busy failed");

    ESP_LOGI(TAG, "e-paper initialized");
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_init(void)
{
    return epaper_waveshare_2in9_v2_init_with_config(&DEFAULT_CONFIG);
}

esp_err_t epaper_waveshare_2in9_v2_init_with_config(const epaper_waveshare_2in9_v2_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        ESP_LOGI(TAG, "e-paper already initialized");
        return ESP_OK;
    }

    s_config = *config;
    ESP_LOGI(TAG, "pin config: MOSI=%d SCLK=%d CS=%d DC=%d RST=%d BUSY=%d PWR=%d SPI=%d Hz timeout=%d ms",
             s_config.mosi_pin, s_config.sclk_pin, s_config.cs_pin,
             s_config.dc_pin, s_config.rst_pin, s_config.busy_pin,
             s_config.pwr_pin, s_config.spi_clock_hz, s_config.busy_timeout_ms);

    uint64_t output_pin_mask = (1ULL << s_config.dc_pin) | (1ULL << s_config.rst_pin);
    if (epaper_has_pwr_pin()) {
        output_pin_mask |= (1ULL << s_config.pwr_pin);
    }

    gpio_config_t output_conf = {
        .pin_bit_mask = output_pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_conf), TAG, "output gpio config failed");
    ESP_RETURN_ON_ERROR(epaper_set_pwr_level(1), TAG, "power gpio high failed");

    gpio_config_t busy_conf = {
        .pin_bit_mask = (1ULL << s_config.busy_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_conf), TAG, "busy gpio config failed");

    spi_bus_config_t bus_config = {
        .mosi_io_num = s_config.mosi_pin,
        .miso_io_num = -1,
        .sclk_io_num = s_config.sclk_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPAPER_2IN9_V2_FB_BYTES,
    };
    esp_err_t err = spi_bus_initialize(MIMI_DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    bool bus_initialized_here = (err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi bus initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = s_config.spi_clock_hz,
        .mode = 0,
        .spics_io_num = s_config.cs_pin,
        .queue_size = 1,
    };
    err = spi_bus_add_device(MIMI_DISPLAY_SPI_HOST, &device_config, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi add device failed: %s", esp_err_to_name(err));
        if (bus_initialized_here) {
            esp_err_t cleanup_err = spi_bus_free(MIMI_DISPLAY_SPI_HOST);
            if (cleanup_err != ESP_OK) {
                ESP_LOGW(TAG, "spi bus cleanup failed: %s", esp_err_to_name(cleanup_err));
            }
        }
        return err;
    }

    err = epaper_panel_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        esp_err_t cleanup_err = spi_bus_remove_device(s_spi);
        if (cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "spi device cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
        s_spi = NULL;
        if (bus_initialized_here) {
            cleanup_err = spi_bus_free(MIMI_DISPLAY_SPI_HOST);
            if (cleanup_err != ESP_OK) {
                ESP_LOGW(TAG, "spi bus cleanup failed: %s", esp_err_to_name(cleanup_err));
            }
        }
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_display_frame(const uint8_t *framebuffer, size_t framebuffer_len)
{
    if (!s_initialized || !s_spi) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!framebuffer || framebuffer_len != EPAPER_2IN9_V2_FB_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "displaying four-color frame (%u bytes)", (unsigned)framebuffer_len);
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_DATA_START_TRANSMISSION), TAG, "write ram cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_data(framebuffer, framebuffer_len), TAG, "write frame data failed");
    ESP_RETURN_ON_ERROR(epaper_turn_on_display(), TAG, "refresh failed");
    ESP_LOGI(TAG, "frame displayed");
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_sleep(void)
{
    if (!s_initialized || !s_spi) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "putting e-paper to sleep");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_POWER_OFF), TAG, "power off cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "power off data failed");
    ESP_RETURN_ON_ERROR(epaper_wait_busy_high("power off"), TAG, "power off busy failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_G_CMD_DEEP_SLEEP), TAG, "deep sleep cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0xA5), TAG, "deep sleep data failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(epaper_set_pwr_level(0), TAG, "power low failed");
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_test_pattern(void)
{
    static uint8_t test_frame[EPAPER_2IN9_V2_FB_BYTES];

    for (size_t i = 0; i < sizeof(test_frame); i++) {
        size_t pixel = i * 4;
        size_t row = pixel / EPAPER_2IN9_V2_WIDTH;
        uint8_t color = EPD_G_COLOR_WHITE;
        if (row < EPAPER_2IN9_V2_HEIGHT / 4) {
            color = EPD_G_COLOR_BLACK;
        } else if (row < EPAPER_2IN9_V2_HEIGHT / 2) {
            color = EPD_G_COLOR_RED;
        } else if (row < (EPAPER_2IN9_V2_HEIGHT * 3) / 4) {
            color = EPD_G_COLOR_YELLOW;
        }
        test_frame[i] = (uint8_t)((color << 6) | (color << 4) | (color << 2) | color);
    }

    ESP_LOGI(TAG, "displaying diagnostic black/red/yellow/white test pattern");
    return epaper_waveshare_2in9_v2_display_frame(test_frame, sizeof(test_frame));
}
