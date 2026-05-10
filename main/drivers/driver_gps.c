#include "driver_gps.h"
#include "sensor_config.h"
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "driver_gps";

/* Internal state */
static gps_reading_t s_reading;
static SemaphoreHandle_t s_reading_mutex = NULL;
static TaskHandle_t s_task_handle = NULL;
static bool s_initialized = false;
static bool s_running = false;

/* NMEA line buffer */
#define NMEA_LINE_BUFFER_SIZE  128
static char s_line_buffer[NMEA_LINE_BUFFER_SIZE];
static int s_line_buffer_idx = 0;

/* UART configuration */
static uart_port_t s_uart_port = UART_NUM_1;

/* Helper: parse a number from NMEA field */
static double parse_double(const char *str)
{
    if (!str || *str == '\0') {
        return 0.0;
    }
    return atof(str);
}

/* Helper: parse latitude/longitude in NMEA format (DDDMM.MMMMM) */
static double parse_lat_lon(const char *str, char hemisphere)
{
    if (!str || *str == '\0') {
        return 0.0;
    }

    double val = parse_double(str);
    int degrees = (int)(val / 100.0);
    double minutes = val - (degrees * 100.0);
    double decimal_degrees = degrees + (minutes / 60.0);

    if (hemisphere == 'S' || hemisphere == 'W') {
        decimal_degrees = -decimal_degrees;
    }

    return decimal_degrees;
}

/* Helper: calculate NMEA checksum */
static bool nmea_checksum_valid(const char *line)
{
    if (!line || line[0] != '$') {
        return false;
    }

    /* Find checksum start */
    const char *star = strchr(line, '*');
    if (!star) {
        return false;
    }

    /* Calculate checksum from $ to * */
    uint8_t calc_checksum = 0;
    for (const char *p = line + 1; p < star; p++) {
        calc_checksum ^= (uint8_t)*p;
    }

    /* Parse received checksum */
    uint8_t recv_checksum = (uint8_t)strtoul(star + 1, NULL, 16);

    return calc_checksum == recv_checksum;
}

/* Helper: extract NMEA fields */
static int nmea_split_fields(char *line, char **fields, int max_fields)
{
    int count = 0;
    fields[count++] = line;

    for (char *p = line; *p && count < max_fields; p++) {
        if (*p == ',' || *p == '*') {
            *p = '\0';
            if (*(p + 1) && *(p + 1) != '*') {
                fields[count++] = p + 1;
            }
        }
    }

    return count;
}

/* Parse GPRMC sentence */
static void parse_gprmc(char *line)
{
    char *fields[20];
    int num_fields = nmea_split_fields(line, fields, 20);

    if (num_fields < 12) {
        return;
    }

    /* Field 2: Status (A = valid, V = invalid) */
    bool fix_valid = (fields[2][0] == 'A');

    /* Only update if we have a valid fix */
    if (!fix_valid) {
        return;
    }

    /* Lock the reading */
    if (xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* Field 3: Latitude */
        if (fields[3][0] != '\0') {
            s_reading.latitude = parse_lat_lon(fields[3], fields[4][0]);
        }

        /* Field 5: Longitude */
        if (fields[5][0] != '\0') {
            s_reading.longitude = parse_lat_lon(fields[5], fields[6][0]);
        }

        /* Field 7: Speed (knots to m/s) */
        if (fields[7][0] != '\0') {
            s_reading.speed_mps = parse_double(fields[7]) * 0.514444;
        }

        /* Field 8: Course (degrees) */
        if (fields[8][0] != '\0') {
            s_reading.course_deg = parse_double(fields[8]);
        }

        s_reading.fix_valid = true;
        s_reading.timestamp_us = esp_timer_get_time();
        xSemaphoreGive(s_reading_mutex);
    }
}

/* Parse GPGGA sentence */
static void parse_gpgga(char *line)
{
    char *fields[20];
    int num_fields = nmea_split_fields(line, fields, 20);

    if (num_fields < 10) {
        return;
    }

    /* Lock the reading */
    if (xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        /* Field 6: Fix quality (0 = invalid, 1 = GPS fix, 2 = DGPS fix) */
        int fix_quality = (fields[6][0] != '\0') ? atoi(fields[6]) : 0;
        s_reading.fix_valid = (fix_quality > 0);

        /* Field 7: Number of satellites */
        if (fields[7][0] != '\0') {
            s_reading.satellites = atoi(fields[7]);
        }

        /* Field 9: Altitude (meters) */
        if (fields[9][0] != '\0') {
            s_reading.altitude_m = parse_double(fields[9]);
        }

        s_reading.timestamp_us = esp_timer_get_time();
        xSemaphoreGive(s_reading_mutex);
    }
}

/* Process a complete NMEA line */
static void process_nmea_line(char *line)
{
    if (!line || strlen(line) < 7) {
        return;
    }

    /* Validate checksum */
    if (!nmea_checksum_valid(line)) {
        ESP_LOGV(TAG, "Checksum invalid: %s", line);
        return;
    }

    /* Check sentence type */
    if (strncmp(line, "$GPRMC", 6) == 0 || strncmp(line, "$GNRMC", 6) == 0) {
        parse_gprmc(line);
    } else if (strncmp(line, "$GPGGA", 6) == 0 || strncmp(line, "$GNGGA", 6) == 0) {
        parse_gpgga(line);
    }
}

/* GPS background task */
static void driver_gps_task(void *arg)
{
    ESP_LOGI(TAG, "GPS task started");

    uint8_t data[128];

    while (s_running) {
        /* Read data from UART */
        int len = uart_read_bytes(s_uart_port, data, sizeof(data) - 1, pdMS_TO_TICKS(100));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];

                if (c == '\n' || c == '\r') {
                    /* End of line, process it */
                    if (s_line_buffer_idx > 0) {
                        s_line_buffer[s_line_buffer_idx] = '\0';
                        process_nmea_line(s_line_buffer);
                        s_line_buffer_idx = 0;
                    }
                } else if (s_line_buffer_idx < NMEA_LINE_BUFFER_SIZE - 1) {
                    /* Add to line buffer */
                    s_line_buffer[s_line_buffer_idx++] = c;
                }
            }
        }
    }

    ESP_LOGI(TAG, "GPS task stopping");
    vTaskDelete(NULL);
}

esp_err_t driver_gps_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing GPS driver");

    /* Get configuration */
    const gps_config_t *config = sensor_config_get_gps();
    if (!config) {
        ESP_LOGE(TAG, "Failed to get GPS config");
        return ESP_ERR_INVALID_STATE;
    }

    /* Check GPIO pins */
    if (!gpio_policy_pin_is_allowed(config->rx_gpio)) {
        ESP_LOGE(TAG, "GPIO %d not allowed for RX", config->rx_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (!gpio_policy_pin_is_allowed(config->tx_gpio)) {
        ESP_LOGE(TAG, "GPIO %d not allowed for TX", config->tx_gpio);
        return ESP_ERR_INVALID_ARG;
    }

    s_uart_port = (uart_port_t)config->uart_port;

    /* Configure UART parameters */
    uart_config_t uart_config = {
        .baud_rate = config->baudrate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    /* Install UART driver */
    esp_err_t err = uart_driver_install(s_uart_port, 1024 * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure UART parameters */
    err = uart_param_config(s_uart_port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
        uart_driver_delete(s_uart_port);
        return err;
    }

    /* Set UART pins */
    err = uart_set_pin(s_uart_port, config->tx_gpio, config->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        uart_driver_delete(s_uart_port);
        return err;
    }

    /* Create mutex for reading protection */
    s_reading_mutex = xSemaphoreCreateMutex();
    if (!s_reading_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        uart_driver_delete(s_uart_port);
        return ESP_ERR_NO_MEM;
    }

    /* Initialize reading to invalid */
    memset(&s_reading, 0, sizeof(s_reading));
    s_reading.fix_valid = false;

    s_initialized = true;
    ESP_LOGI(TAG, "GPS driver initialized");
    return ESP_OK;
}

esp_err_t driver_gps_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting GPS task");

    /* Create task on Core 0 (same as UART/network tasks) */
    s_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        driver_gps_task,
        "gps_task",
        4 * 1024,
        NULL,
        4,  /* Priority */
        &s_task_handle,
        0   /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create task");
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t driver_gps_stop(void)
{
    if (!s_running) {
        ESP_LOGW(TAG, "Not running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping GPS task");

    s_running = false;

    /* Wait for task to exit (should be fast, it checks s_running) */
    vTaskDelay(pdMS_TO_TICKS(200));

    s_task_handle = NULL;
    return ESP_OK;
}

gps_reading_t driver_gps_get_reading(void)
{
    gps_reading_t reading;

    /* Lock and copy */
    if (xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        reading = s_reading;
        xSemaphoreGive(s_reading_mutex);
    } else {
        memset(&reading, 0, sizeof(reading));
        reading.fix_valid = false;
    }

    return reading;
}
