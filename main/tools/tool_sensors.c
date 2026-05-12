#include "tools/tool_sensors.h"
#include "drivers/driver_ultrasonic.h"
#include "drivers/driver_imu.h"
#include "drivers/driver_gps.h"
#include "drivers/sensor_config.h"
#include "nav/nav_gps_filter.h"

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

    err = driver_imu_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU driver init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = driver_imu_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU driver start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = driver_gps_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPS driver init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = driver_gps_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPS driver start failed: %s", esp_err_to_name(err));
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

static esp_err_t print_imu_reading(char *output, size_t output_size)
{
    imu_reading_t reading = driver_imu_get_reading();

    snprintf(output, output_size,
             "Roll:%.1f° Pitch:%.1f° Yaw:%.1f° "
             "Gx:%.1f Gy:%.1f Gz:%.1f "
             "[V:%d] "
             "Age:%lldms",
             reading.roll_deg, reading.pitch_deg, reading.yaw_deg,
             reading.gyro_dps[0], reading.gyro_dps[1], reading.gyro_dps[2],
             reading.valid ? 1 : 0,
             (long long)((esp_timer_get_time() - reading.timestamp_us) / 1000));

    return ESP_OK;
}

esp_err_t tool_imu_test_execute(const char *input_json, char *output, size_t output_size)
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
    snprintf(output, output_size, "MPU6050 Test Started: continuous=%d, count=%d, delay=%dms",
             continuous, count, delay_ms);
    ESP_LOGI(TAG, "%s", output);

    /* Single reading test */
    if (!continuous) {
        for (int i = 0; i < count; i++) {
            char buf[200];
            print_imu_reading(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s", buf);
            if (i < count - 1) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }

        print_imu_reading(output, output_size);
        return ESP_OK;
    }

    /* Continuous test (will never return in CLI context) */
    snprintf(output, output_size, "Continuous test running (will not return)...");
    while (1) {
        char buf[200];
        print_imu_reading(buf, sizeof(buf));
        ESP_LOGI(TAG, "%s", buf);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return ESP_OK;
}

static esp_err_t print_gps_reading(char *output, size_t output_size)
{
    gps_reading_t reading = driver_gps_get_reading();

    snprintf(output, output_size,
             "Lat:%.6f Lon:%.6f "
             "Alt:%.1fm Spd:%.1fm/s Crs:%.0f° "
             "Sats:%d "
             "[V:%d] "
             "Age:%lldms",
             reading.latitude, reading.longitude,
             reading.altitude_m, reading.speed_mps, reading.course_deg,
             reading.satellites,
             reading.fix_valid ? 1 : 0,
             (long long)((esp_timer_get_time() - reading.timestamp_us) / 1000));

    return ESP_OK;
}

esp_err_t tool_gps_test_execute(const char *input_json, char *output, size_t output_size)
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
    snprintf(output, output_size, "GPS Test Started: continuous=%d, count=%d, delay=%dms",
             continuous, count, delay_ms);
    ESP_LOGI(TAG, "%s", output);

    /* Single reading test */
    if (!continuous) {
        for (int i = 0; i < count; i++) {
            char buf[250];
            print_gps_reading(buf, sizeof(buf));
            ESP_LOGI(TAG, "%s", buf);
            if (i < count - 1) {
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        }

        print_gps_reading(output, output_size);
        return ESP_OK;
    }

    /* Continuous test (will never return in CLI context) */
    snprintf(output, output_size, "Continuous test running (will not return)...");
    while (1) {
        char buf[250];
        print_gps_reading(buf, sizeof(buf));
        ESP_LOGI(TAG, "%s", buf);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return ESP_OK;
}

/* --- LLM-facing read tools (single snapshot, structured JSON) --- */

esp_err_t tool_read_distance_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    ultrasonic_reading_t left, front, right;
    driver_ultrasonic_get_all(&left, &front, &right);

    int64_t age_ms = (esp_timer_get_time() - front.timestamp_us) / 1000;

    snprintf(output, output_size,
             "{\"left_cm\":%d,\"front_cm\":%d,\"right_cm\":%d,"
             "\"valid\":{\"left\":%s,\"front\":%s,\"right\":%s},"
             "\"age_ms\":%lld}",
             left.distance_cm, front.distance_cm, right.distance_cm,
             left.valid  ? "true" : "false",
             front.valid ? "true" : "false",
             right.valid ? "true" : "false",
             (long long)age_ms);
    return ESP_OK;
}

esp_err_t tool_read_imu_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    imu_reading_t r = driver_imu_get_reading();
    int64_t age_ms  = (esp_timer_get_time() - r.timestamp_us) / 1000;

    snprintf(output, output_size,
             "{\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f,"
             "\"gz_dps\":%.2f,\"valid\":%s,\"age_ms\":%lld}",
             r.roll_deg, r.pitch_deg, r.yaw_deg,
             r.gyro_dps[2],
             r.valid ? "true" : "false",
             (long long)age_ms);
    return ESP_OK;
}

esp_err_t tool_read_gps_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    gps_reading_t r = driver_gps_get_reading();
    nav_gps_filtered_t f;
    nav_gps_filter_get_last(&f);

    int64_t raw_age_ms  = (esp_timer_get_time() - r.timestamp_us) / 1000;
    int64_t filt_age_ms = (esp_timer_get_time() - f.ts_us) / 1000;

    snprintf(output, output_size,
             "{\"raw\":{\"lat\":%.6f,\"lon\":%.6f,\"fix\":%s,\"sats\":%d,"
             "\"speed_mps\":%.2f,\"course_deg\":%.1f,\"age_ms\":%lld},"
             "\"filt\":{\"lat\":%.6f,\"lon\":%.6f,\"fix\":%s,\"sats\":%d,"
             "\"speed_mps\":%.2f,\"course_deg\":%.1f,"
             "\"quality\":%d,\"age_ms\":%lld}}",
             r.latitude, r.longitude,
             r.fix_valid ? "true" : "false",
             r.satellites, r.speed_mps, r.course_deg,
             (long long)raw_age_ms,
             f.lat, f.lon,
             f.fix ? "true" : "false",
             f.sats, f.speed_mps, f.course_deg,
             f.quality, (long long)filt_age_ms);
    return ESP_OK;
}
