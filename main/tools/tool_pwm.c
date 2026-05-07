#include "tools/tool_pwm.h"
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_pwm";

/* Servo timing (standard 50 Hz servo) */
#define SERVO_FREQ_HZ         50
#define SERVO_DUTY_RES        LEDC_TIMER_14_BIT  /* 16383 max */
#define SERVO_PERIOD_US       (1000000 / SERVO_FREQ_HZ)  /* 20000 us */

/* Pulse width range in microseconds */
#define SERVO_MIN_PULSE_US    500    /* 0° */
#define SERVO_MAX_PULSE_US    2500   /* 180° */

/* Max concurrent servo channels (ESP32-S3 LEDC has 8 channels) */
#define MAX_SERVO_CHANNELS    8

typedef struct {
    int gpio;                   /* -1 = slot free */
    ledc_channel_t channel;
} servo_slot_t;

static servo_slot_t s_servos[MAX_SERVO_CHANNELS];
static bool s_timer_configured = false;
static int s_next_channel = 0;

static inline int angle_to_duty(int angle)
{
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    int pulse_us = SERVO_MIN_PULSE_US +
        (int)((int64_t)(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * angle / 180);
    return (int)((int64_t)pulse_us * ((1 << 14) - 1) / SERVO_PERIOD_US);
}

static servo_slot_t *find_slot_by_gpio(int gpio)
{
    for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
        if (s_servos[i].gpio == gpio) return &s_servos[i];
    }
    return NULL;
}

static servo_slot_t *alloc_slot(int gpio)
{
    if (s_next_channel >= MAX_SERVO_CHANNELS) return NULL;
    servo_slot_t *slot = &s_servos[s_next_channel];
    slot->gpio = gpio;
    slot->channel = (ledc_channel_t)s_next_channel;
    s_next_channel++;
    return slot;
}

static esp_err_t ensure_timer(void)
{
    if (s_timer_configured) return ESP_OK;

    ledc_timer_config_t cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_DUTY_RES,
        .timer_num       = MIMI_PWM_LEDC_TIMER,
        .freq_hz         = SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_timer_configured = true;
    ESP_LOGI(TAG, "LEDC timer configured: %d Hz, timer %d", SERVO_FREQ_HZ, MIMI_PWM_LEDC_TIMER);
    return ESP_OK;
}

esp_err_t tool_pwm_init(void)
{
    for (int i = 0; i < MAX_SERVO_CHANNELS; i++) {
        s_servos[i].gpio = -1;
    }
    s_next_channel = 0;
    s_timer_configured = false;
    ESP_LOGI(TAG, "PWM/Servo tool initialized (max %d channels)", MAX_SERVO_CHANNELS);
    return ESP_OK;
}

esp_err_t tool_servo_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *gpio_obj = cJSON_GetObjectItem(root, "gpio");
    cJSON *angle_obj = cJSON_GetObjectItem(root, "angle");

    if (!cJSON_IsNumber(gpio_obj)) {
        snprintf(output, output_size, "Error: 'gpio' required (integer pin number)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(angle_obj)) {
        snprintf(output, output_size, "Error: 'angle' required (0-180 degrees)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int gpio = (int)gpio_obj->valuedouble;
    int angle = (int)angle_obj->valuedouble;

    if (!gpio_policy_pin_is_allowed(gpio)) {
        if (gpio_policy_pin_forbidden_hint(gpio, output, output_size)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(output, output_size, "Error: GPIO %d is not in allowed list", gpio);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (angle < 0 || angle > 180) {
        snprintf(output, output_size, "Error: angle must be 0-180, got %d", angle);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_timer();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: PWM timer init failed");
        cJSON_Delete(root);
        return err;
    }

    servo_slot_t *slot = find_slot_by_gpio(gpio);
    if (!slot) {
        slot = alloc_slot(gpio);
        if (!slot) {
            snprintf(output, output_size,
                     "Error: max %d servo channels reached, release one first",
                     MAX_SERVO_CHANNELS);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        ledc_channel_config_t ch = {
            .gpio_num   = gpio,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = slot->channel,
            .intr_type  = LEDC_INTR_DISABLE,
            .timer_sel  = MIMI_PWM_LEDC_TIMER,
            .duty       = 0,
            .hpoint     = 0,
        };
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) {
            slot->gpio = -1;
            s_next_channel--;
            snprintf(output, output_size, "Error: LEDC channel config failed on GPIO %d", gpio);
            cJSON_Delete(root);
            return err;
        }
    }

    int duty = angle_to_duty(angle);
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->channel, duty);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set duty on GPIO %d", gpio);
        cJSON_Delete(root);
        return err;
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);

    snprintf(output, output_size, "Servo on GPIO %d set to %d°", gpio, angle);
    ESP_LOGI(TAG, "servo_set: GPIO %d -> %d° (duty=%d)", gpio, angle, duty);

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_servo_release_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *gpio_obj = cJSON_GetObjectItem(root, "gpio");
    if (!cJSON_IsNumber(gpio_obj)) {
        snprintf(output, output_size, "Error: 'gpio' required (integer pin number)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int gpio = (int)gpio_obj->valuedouble;

    servo_slot_t *slot = find_slot_by_gpio(gpio);
    if (!slot) {
        snprintf(output, output_size, "No active servo on GPIO %d", gpio);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    slot->gpio = -1;

    snprintf(output, output_size, "Servo on GPIO %d released", gpio);
    ESP_LOGI(TAG, "servo_release: GPIO %d released", gpio);

    cJSON_Delete(root);
    return ESP_OK;
}
