#include "display/epaper_waveshare_2in9_v2.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "epaper_2in9_v2";

#define EPD_CMD_DRIVER_OUTPUT_CONTROL          0x01
#define EPD_CMD_GATE_DRIVING_VOLTAGE           0x03
#define EPD_CMD_SOURCE_DRIVING_VOLTAGE         0x04
#define EPD_CMD_DATA_ENTRY_MODE                0x11
#define EPD_CMD_SW_RESET                       0x12
#define EPD_CMD_MASTER_ACTIVATION              0x20
#define EPD_CMD_DISPLAY_UPDATE_CONTROL_2       0x22
#define EPD_CMD_WRITE_RAM_BW                   0x24
#define EPD_CMD_BORDER_WAVEFORM                0x3C
#define EPD_CMD_SET_RAM_X_ADDRESS_START_END    0x44
#define EPD_CMD_SET_RAM_Y_ADDRESS_START_END    0x45
#define EPD_CMD_SET_RAM_X_ADDRESS_COUNTER      0x4E
#define EPD_CMD_SET_RAM_Y_ADDRESS_COUNTER      0x4F

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
    .spi_clock_hz = MIMI_DISPLAY_SPI_CLOCK_HZ,
    .busy_timeout_ms = MIMI_DISPLAY_BUSY_TIMEOUT_MS,
};

static esp_err_t epaper_wait_busy(const char *stage)
{
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(s_config.busy_timeout_ms);

    while (gpio_get_level(s_config.busy_pin) == 1) {
        if ((xTaskGetTickCount() - start) >= timeout) {
            ESP_LOGE(TAG, "busy timeout during %s after %d ms", stage, s_config.busy_timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

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
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 0), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(2));
    ESP_RETURN_ON_ERROR(gpio_set_level(s_config.rst_pin, 1), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static esp_err_t epaper_set_memory_area(void)
{
    const uint16_t y_end = EPAPER_2IN9_V2_HEIGHT - 1;

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SET_RAM_X_ADDRESS_START_END), TAG, "set x area cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set x start failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8((EPAPER_2IN9_V2_WIDTH / 8) - 1), TAG, "set x end failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SET_RAM_Y_ADDRESS_START_END), TAG, "set y area cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set y start low failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set y start high failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8((uint8_t)(y_end & 0xFF)), TAG, "set y end low failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8((uint8_t)(y_end >> 8)), TAG, "set y end high failed");

    return ESP_OK;
}

static esp_err_t epaper_set_memory_pointer(void)
{
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SET_RAM_X_ADDRESS_COUNTER), TAG, "set x counter cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set x counter failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SET_RAM_Y_ADDRESS_COUNTER), TAG, "set y counter cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set y counter low failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "set y counter high failed");

    return epaper_wait_busy("set memory pointer");
}

static esp_err_t epaper_power_on(void)
{
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2), TAG, "power on update cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0xC0), TAG, "power on update data failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_MASTER_ACTIVATION), TAG, "power on activate failed");
    return epaper_wait_busy("power on");
}

static esp_err_t epaper_refresh(void)
{
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2), TAG, "refresh update cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0xC7), TAG, "refresh update data failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_MASTER_ACTIVATION), TAG, "refresh activate failed");
    return epaper_wait_busy("display refresh");
}

static esp_err_t epaper_panel_init(void)
{
    const uint16_t height_minus_one = EPAPER_2IN9_V2_HEIGHT - 1;

    ESP_LOGI(TAG, "initializing Waveshare 2.9in V2 e-paper");
    ESP_RETURN_ON_ERROR(epaper_reset(), TAG, "hardware reset failed");
    ESP_RETURN_ON_ERROR(epaper_wait_busy("post reset"), TAG, "post reset busy failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SW_RESET), TAG, "sw reset cmd failed");
    ESP_RETURN_ON_ERROR(epaper_wait_busy("software reset"), TAG, "sw reset busy failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_DRIVER_OUTPUT_CONTROL), TAG, "driver output cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8((uint8_t)(height_minus_one & 0xFF)), TAG, "driver output low failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8((uint8_t)(height_minus_one >> 8)), TAG, "driver output high failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "driver output scan failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_DATA_ENTRY_MODE), TAG, "data entry cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x03), TAG, "data entry data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_BORDER_WAVEFORM), TAG, "border cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x05), TAG, "border data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_GATE_DRIVING_VOLTAGE), TAG, "gate voltage cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x17), TAG, "gate voltage data failed");

    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_SOURCE_DRIVING_VOLTAGE), TAG, "source voltage cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x41), TAG, "source voltage data 1 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x00), TAG, "source voltage data 2 failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x32), TAG, "source voltage data 3 failed");

    ESP_RETURN_ON_ERROR(epaper_set_memory_area(), TAG, "set memory area failed");
    ESP_RETURN_ON_ERROR(epaper_power_on(), TAG, "power on failed");

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

    gpio_config_t output_conf = {
        .pin_bit_mask = (1ULL << s_config.dc_pin) | (1ULL << s_config.rst_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_conf), TAG, "output gpio config failed");

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

    ESP_LOGI(TAG, "displaying full frame (%u bytes)", (unsigned)framebuffer_len);
    ESP_RETURN_ON_ERROR(epaper_set_memory_pointer(), TAG, "set memory pointer failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_WRITE_RAM_BW), TAG, "write ram cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_data(framebuffer, framebuffer_len), TAG, "write frame data failed");
    ESP_RETURN_ON_ERROR(epaper_refresh(), TAG, "refresh failed");
    ESP_LOGI(TAG, "frame displayed");
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_sleep(void)
{
    if (!s_initialized || !s_spi) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "putting e-paper to sleep");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_DISPLAY_UPDATE_CONTROL_2), TAG, "power off update cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x83), TAG, "power off update data failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(EPD_CMD_MASTER_ACTIVATION), TAG, "power off activate failed");
    ESP_RETURN_ON_ERROR(epaper_wait_busy("power off"), TAG, "power off busy failed");
    ESP_RETURN_ON_ERROR(epaper_write_command(0x10), TAG, "deep sleep cmd failed");
    ESP_RETURN_ON_ERROR(epaper_write_u8(0x01), TAG, "deep sleep data failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t epaper_waveshare_2in9_v2_test_pattern(void)
{
    static uint8_t test_frame[EPAPER_2IN9_V2_FB_BYTES];

    for (size_t i = 0; i < sizeof(test_frame); i++) {
        test_frame[i] = (i % 2 == 0) ? 0xAA : 0x55;
    }

    return epaper_waveshare_2in9_v2_display_frame(test_frame, sizeof(test_frame));
}
