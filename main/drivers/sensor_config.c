#include "drivers/sensor_config.h"
#include "mimi_config.h"
#include "tools/gpio_policy.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "sensor_config";

#define SENSORS_CONFIG_PATH   MIMI_SPIFFS_BASE "/config/sensors.json"

/* Defaults for HC-SR04 (as per specification) */
#define US_LEFT_TRIG_DEF     10
#define US_LEFT_ECHO_DEF     12
#define US_FRONT_TRIG_DEF    13
#define US_FRONT_ECHO_DEF    14
#define US_RIGHT_TRIG_DEF   15
#define US_RIGHT_ECHO_DEF      16
#define US_MAX_RANGE_DEF    400
#define US_ROUND_GAP_DEF    60

static ultrasonic_config_t s_ultrasonic;
static bool s_initialized = false;

static void ultrasonic_load_defaults(void)
{
    s_ultrasonic.left.trig_gpio = US_LEFT_TRIG_DEF;
    s_ultrasonic.left.echo_gpio = US_LEFT_ECHO_DEF;
    s_ultrasonic.front.trig_gpio = US_FRONT_TRIG_DEF;
    s_ultrasonic.front.echo_gpio = US_FRONT_ECHO_DEF;
    s_ultrasonic.right.trig_gpio = US_RIGHT_TRIG_DEF;
    s_ultrasonic.right.echo_gpio = US_RIGHT_ECHO_DEF;
    s_ultrasonic.max_range_cm = US_MAX_RANGE_DEF;
    s_ultrasonic.round_robin_gap_ms = US_ROUND_GAP_DEF;
    s_ultrasonic.loaded = false;
}

esp_err_t sensor_config_load(void)
{
    ultrasonic_load_defaults();

    FILE *f = fopen(SENSORS_CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No sensors.json found, using defaults");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) {
        fclose(f);
        ESP_LOGW(TAG, "sensors.json size out of range, using defaults");
        return ESP_OK;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "malloc failed for sensors.json");
        return ESP_ERR_NO_MEM;
    }
    buf[fread(buf, 1, len, f)] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "sensors.json parse failed, using defaults");
        return ESP_OK;
    }

    cJSON *ultrasonic = cJSON_GetObjectItem(root, "ultrasonic");
    if (ultrasonic) {
        cJSON *v;
        cJSON *left = cJSON_GetObjectItem(ultrasonic, "left");
        if (left) {
            v = cJSON_GetObjectItem(left, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.left.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(left, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.left.echo_gpio = v->valueint;
        }
        cJSON *front = cJSON_GetObjectItem(ultrasonic, "front");
        if (front) {
            v = cJSON_GetObjectItem(front, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.front.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(front, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.front.echo_gpio = v->valueint;
        }
        cJSON *right = cJSON_GetObjectItem(ultrasonic, "right");
        if (right) {
            v = cJSON_GetObjectItem(right, "trig_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.right.trig_gpio = v->valueint;
            v = cJSON_GetObjectItem(right, "echo_gpio");
            if (cJSON_IsNumber(v)) s_ultrasonic.right.echo_gpio = v->valueint;
        }
        v = cJSON_GetObjectItem(ultrasonic, "max_range_cm");
        if (cJSON_IsNumber(v)) s_ultrasonic.max_range_cm = v->valueint;
        v = cJSON_GetObjectItem(ultrasonic, "round_robin_gap_ms");
        if (cJSON_IsNumber(v)) s_ultrasonic.round_robin_gap_ms = v->valueint;
    }

    cJSON_Delete(root);
    s_ultrasonic.loaded = true;
    ESP_LOGI(TAG, "Ultrasonic config loaded: L(GPIO%d/%d F(GPIO%d/%d) R(GPIO%d/%d) max=%dcm gap=%dms",
             s_ultrasonic.left.trig_gpio, s_ultrasonic.left.echo_gpio,
             s_ultrasonic.front.trig_gpio, s_ultrasonic.front.echo_gpio,
             s_ultrasonic.right.trig_gpio, s_ultrasonic.right.echo_gpio,
             s_ultrasonic.max_range_cm, s_ultrasonic.round_robin_gap_ms);
    return ESP_OK;
}

const ultrasonic_config_t *sensor_config_get_ultrasonic(void)
{
    return &s_ultrasonic;
}

esp_err_t sensor_config_init(void)
{
    if (!s_initialized) {
        ultrasonic_load_defaults();
        sensor_config_load();
        s_initialized = true;
        ESP_LOGI(TAG, "sensor config initialized");
    }
    return ESP_OK;
}
