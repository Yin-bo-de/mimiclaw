#include "tools/tool_sensors.h"
#include "drivers/driver_ultrasonic.h"
#include "drivers/sensor_config.h"

#include "tools/tool_registry.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_sensors";

esp_err_t tool_sensors_init(void)
{
    esp_err_t err = sensor_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor config init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = driver_ultrasonic_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ultrasonic driver init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = driver_ultrasonic_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ultrasonic driver start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "tool_sensors initialized");
    return ESP_OK;
}

static esp_err_t print_readings(char *output, size_t output_size)
{
    ultrasonic_reading_t left, front, right;
    driver_ultrasonic_get_all(&left, &front, &right);

    snprintf(output, output_size,
             "L:%3dcm F:%3dcm R:%3dcm "
             "[V:%d,%d,%d] "
             "Age:%lldms",
             left.distance_cm, front.distance_cm, right.distance_cm,
             left.valid ? 1 : 0, front.valid ? 1 : 0, right.valid ? 1 : 0,
             (long long)((esp_timer_get_time() - front.timestamp_us) / 1000));

    return ESP_OK;
}

esp_err_t tool_ultrasonic_test_execute(const char *input_json, char *output, size_t output_size)
{
    bool continuous = false;
    int count = 10;
    int delay_ms = 1000;

    /* Parse optional input parameters */
    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *v;
            v = cJSON_GetObjectItem(root, "continuous");
            if (cJSON_IsBool(v)) continuous = cJSON_IsTrue(v);
            v = cJSON_GetObjectItem(root, "count");
            if (cJSON_IsNumber(v) && v->valuedouble > 0) count = (int)v->valuedouble;
            v = cJSON_GetObjectItem(root, "delay_ms");
            if (cJSON_IsNumber(v) && v->valuedouble > 0) delay_ms = (int)v->valuedouble;
            cJSON_Delete(root);
        }
    }

    /* Print header */
    snprintf(output, output_size, "HC-SR04 Test Started: continuous=%d, count=%d, delay=%dms",
             continuous, count, delay_ms);
    ESP_LOGI(TAG, "%s", output);

    /* Single reading test */
    if (!continuous) {
        for (int i = 0; i < count; i++) {
            char buf[128];
            print_readings(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s", buf);
            if (i < count - 1) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }

        print_readings(output, output_size);
        return ESP_OK;
    }

    /* Continuous test (will never return in CLI context) */
    snprintf(output, output_size, "Continuous test running (will not return)...");
    while (1) {
        char buf[128];
        print_readings(buf, sizeof(buf));
        ESP_LOGI(TAG, "%s", buf);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return ESP_OK;
}
