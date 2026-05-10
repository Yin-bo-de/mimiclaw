#include "drivers/driver_ultrasonic.h"
#include "drivers/sensor_config.h"
#include "nav/nav_situation.h"

#include "mimi_config.h"
#include "tools/gpio_policy.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "driver_ultrasonic";

/* Timing constants (HC-SR04) */
#define US_TRIG_PULSE_US        10
#define US_MIN_ECHO_US          100
#define US_MAX_ECHO_US          25000
#define US_TIMEOUT_US           30000

static int s_trig_gpios[ULTRASONIC_CHANNEL_COUNT];
static int s_echo_gpios[ULTRASONIC_CHANNEL_COUNT];
static ultrasonic_reading_t s_readings[ULTRASONIC_CHANNEL_COUNT];
static SemaphoreHandle_t s_reading_mutex = NULL;
static TaskHandle_t s_ultrasonic_task = NULL;
static bool s_task_running = false;
static bool s_initialized = false;

static const ultrasonic_config_t *s_config;

/* Send trigger pulse */
static void send_trigger(int trig_gpio)
{
    gpio_set_level(trig_gpio, 0);
    esp_rom_delay_us(2);
    gpio_set_level(trig_gpio, 1);
    esp_rom_delay_us(US_TRIG_PULSE_US);
    gpio_set_level(trig_gpio, 0);
}

/* Measure echo on a single channel, returns distance in cm.
 * -1  = hardware fault / abnormal echo (too short)
 * >=2 = actual measured distance
 * When no echo is received (open space) or echo exceeds max_range,
 * returns max_range to indicate "clear ahead" rather than "sensor failed".
 */
static int measure_single_channel(int trig_gpio, int echo_gpio, int max_range)
{
    send_trigger(trig_gpio);

    int64_t start = esp_timer_get_time();
    int64_t timeout = start + US_TIMEOUT_US;

    /* Wait for echo to go high */
    while (gpio_get_level(echo_gpio) == 0) {
        if (esp_timer_get_time() > timeout) {
            /* No echo = open space, treat as max range (clear) */
            return max_range;
        }
    }

    int64_t echo_start = esp_timer_get_time();
    timeout = echo_start + US_MAX_ECHO_US;

    /* Wait for echo to go low */
    while (gpio_get_level(echo_gpio) == 1) {
        if (esp_timer_get_time() > timeout) {
            /* Echo too long = beyond max range, treat as max range (clear) */
            return max_range;
        }
    }

    int64_t echo_end = esp_timer_get_time();
    int64_t duration = echo_end - echo_start;

    if (duration < US_MIN_ECHO_US) {
        /* Abnormally short echo — real hardware fault */
        return -1;
    }

    /* Distance = (sound_speed * time) / 2, sound_speed = ~343 m/s
     * distance_cm = (34300 cm/s * duration_s) / 2 = (duration_us / 2) / 29.1
     * Simplified to: duration_us / 58
     */
    int distance = (int)(duration / 58);

    if (distance > max_range) {
        return max_range;
    }

    return distance;
}

static void ultrasonic_task(void *arg)
{
    ESP_LOGI(TAG, "ultrasonic task running");
    const int loop_delay = s_config->round_robin_gap_ms;

    while (s_task_running) {
        for (int ch = 0; ch < ULTRASONIC_CHANNEL_COUNT; ch++) {
            int distance = measure_single_channel(
                s_trig_gpios[ch],
                s_echo_gpios[ch],
                s_config->max_range_cm
            );
            int64_t now = esp_timer_get_time();

            if (s_reading_mutex) {
                if (xSemaphoreTake(s_reading_mutex, portMAX_DELAY) == pdTRUE) {
                    s_readings[ch].distance_cm = distance;
                    s_readings[ch].timestamp_us = now;
                    s_readings[ch].valid = (distance >= 2 && distance <= s_config->max_range_cm);
                    xSemaphoreGive(s_reading_mutex);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(loop_delay));
        }

        /* Publish all three channels to nav_situation after each round */
        ultrasonic_reading_t left, front, right;
        driver_ultrasonic_get_all(&left, &front, &right);
        nav_situation_update_distances(
            left.distance_cm, front.distance_cm, right.distance_cm,
            left.valid, front.valid, right.valid
        );
    }

    vTaskDelete(NULL);
}

esp_err_t driver_ultrasonic_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    s_config = sensor_config_get_ultrasonic();

    /* Initialize GPIO config */
    s_trig_gpios[ULTRASONIC_LEFT] = s_config->left.trig_gpio;
    s_echo_gpios[ULTRASONIC_LEFT] = s_config->left.echo_gpio;
    s_trig_gpios[ULTRASONIC_FRONT] = s_config->front.trig_gpio;
    s_echo_gpios[ULTRASONIC_FRONT] = s_config->front.echo_gpio;
    s_trig_gpios[ULTRASONIC_RIGHT] = s_config->right.trig_gpio;
    s_echo_gpios[ULTRASONIC_RIGHT] = s_config->right.echo_gpio;

    /* Validate GPIOs */
    for (int ch = 0; ch < ULTRASONIC_CHANNEL_COUNT; ch++) {
        if (!gpio_policy_pin_is_allowed(s_trig_gpios[ch]) ||
            !gpio_policy_pin_is_allowed(s_echo_gpios[ch])) {
            ESP_LOGE(TAG, "GPIO %d/%d not in allowed list", s_trig_gpios[ch], s_echo_gpios[ch]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Configure GPIO */
    for (int ch = 0; ch < ULTRASONIC_CHANNEL_COUNT; ch++) {
        gpio_reset_pin(s_trig_gpios[ch]);
        gpio_set_direction(s_trig_gpios[ch], GPIO_MODE_OUTPUT);
        gpio_set_level(s_trig_gpios[ch], 0);

        gpio_reset_pin(s_echo_gpios[ch]);
        gpio_set_direction(s_echo_gpios[ch], GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_echo_gpios[ch], GPIO_FLOATING);
    }

    s_reading_mutex = xSemaphoreCreateMutex();
    if (!s_reading_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    for (int ch = 0; ch < ULTRASONIC_CHANNEL_COUNT; ch++) {
        s_readings[ch].distance_cm = -1;
        s_readings[ch].timestamp_us = 0;
        s_readings[ch].valid = false;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "driver_ultrasonic_init complete");
    return ESP_OK;
}

esp_err_t driver_ultrasonic_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_task_running) {
        s_task_running = true;

        BaseType_t ok = xTaskCreatePinnedToCore(
            ultrasonic_task,
            "ultrasonic",
            4 * 1024,
            NULL,
            5,
            &s_ultrasonic_task,
            1
        );

        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create ultrasonic task");
            s_task_running = false;
            return ESP_ERR_NO_MEM;
        }

        ESP_LOGI(TAG, "driver_ultrasonic_start complete");
    }

    return ESP_OK;
}

esp_err_t driver_ultrasonic_stop(void)
{
    if (s_task_running) {
        s_task_running = false;
        if (s_ultrasonic_task) {
            vTaskDelete(s_ultrasonic_task);
            s_ultrasonic_task = NULL;
        }
        ESP_LOGI(TAG, "driver_ultrasonic_stop complete");
    }

    return ESP_OK;
}

ultrasonic_reading_t driver_ultrasonic_get_reading(ultrasonic_channel_t ch)
{
    if (ch >= ULTRASONIC_CHANNEL_COUNT) {
        ultrasonic_reading_t invalid = { -1, 0, false };
        return invalid;
    }

    ultrasonic_reading_t result;
    if (s_reading_mutex && xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        result = s_readings[ch];
        xSemaphoreGive(s_reading_mutex);
    } else {
        result.distance_cm = -1;
        result.timestamp_us = 0;
        result.valid = false;
    }
    return result;
}

void driver_ultrasonic_get_all(ultrasonic_reading_t *left,
                               ultrasonic_reading_t *front,
                               ultrasonic_reading_t *right)
{
    if (s_reading_mutex && xSemaphoreTake(s_reading_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (left) *left = s_readings[ULTRASONIC_LEFT];
        if (front) *front = s_readings[ULTRASONIC_FRONT];
        if (right) *right = s_readings[ULTRASONIC_RIGHT];
        xSemaphoreGive(s_reading_mutex);
    } else {
        if (left) { left->distance_cm = -1; left->valid = false; }
        if (front) { front->distance_cm = -1; front->valid = false; }
        if (right) { right->distance_cm = -1; right->valid = false; }
    }
}

int driver_ultrasonic_measure_once(ultrasonic_channel_t ch)
{
    if (ch >= ULTRASONIC_CHANNEL_COUNT) return -1;

    int distance = measure_single_channel(
        s_trig_gpios[ch],
        s_echo_gpios[ch],
        s_config->max_range_cm
    );
    int64_t now = esp_timer_get_time();

    if (s_reading_mutex) {
        if (xSemaphoreTake(s_reading_mutex, portMAX_DELAY) == pdTRUE) {
            s_readings[ch].distance_cm = distance;
            s_readings[ch].timestamp_us = now;
            s_readings[ch].valid = (distance >= 2 && distance <= s_config->max_range_cm);
            xSemaphoreGive(s_reading_mutex);
        }
    }

    return distance;
}
