#include "tools/tool_pwm.h"
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_pwm";

/* LEDC / PWM hardware constants */
#define PWM_FREQ_HZ           50
#define PWM_DUTY_RES          LEDC_TIMER_14_BIT  /* 16383 max */
#define PWM_PERIOD_US         (1000000 / PWM_FREQ_HZ)  /* 20000 us */
#define MAX_PWM_CHANNELS      8

/* RC config file path */
#define RC_CONFIG_PATH        MIMI_SPIFFS_BASE "/config/rc.json"

/* RC calibration defaults (standard servo / ESC) */
#define RC_STEER_GPIO_DEF     4
#define RC_STEER_CENTER_US    1500
#define RC_STEER_MIN_US       1000
#define RC_STEER_MAX_US       2000
#define RC_THROTTLE_GPIO_DEF  5
#define RC_THROTTLE_NEUTRAL   1500
#define RC_THROTTLE_FWD_US    2000
#define RC_THROTTLE_REV_US    1000

typedef struct {
    int gpio;                   /* -1 = slot free */
    ledc_channel_t channel;
} pwm_slot_t;

typedef struct {
    int steer_gpio;
    int steer_center_us;
    int steer_min_us;
    int steer_max_us;
    bool steer_reversed;
    int throttle_gpio;
    int throttle_neutral_us;
    int throttle_forward_us;
    int throttle_reverse_us;
    bool throttle_reversed;
    bool loaded;                /* whether rc.json was read successfully */
} rc_config_t;

static pwm_slot_t s_slots[MAX_PWM_CHANNELS];
static rc_config_t s_rc;
static bool s_timer_configured = false;
static int s_next_channel = 0;

/* --- Internal helpers --- */

static int pulse_us_to_duty(int pulse_us)
{
    if (pulse_us < 0) pulse_us = 0;
    if (pulse_us > PWM_PERIOD_US) pulse_us = PWM_PERIOD_US;
    return (int)((int64_t)pulse_us * ((1 << 14) - 1) / PWM_PERIOD_US);
}

static pwm_slot_t *find_slot_by_gpio(int gpio)
{
    for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
        if (s_slots[i].gpio == gpio) return &s_slots[i];
    }
    return NULL;
}

static pwm_slot_t *alloc_slot(int gpio)
{
    if (s_next_channel >= MAX_PWM_CHANNELS) return NULL;
    pwm_slot_t *slot = &s_slots[s_next_channel];
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
        .duty_resolution = PWM_DUTY_RES,
        .timer_num       = MIMI_PWM_LEDC_TIMER,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_timer_configured = true;
    ESP_LOGI(TAG, "LEDC timer configured: %d Hz, timer %d", PWM_FREQ_HZ, MIMI_PWM_LEDC_TIMER);
    return ESP_OK;
}

/* Set PWM pulse width on a GPIO. Allocates channel if needed. */
static esp_err_t pwm_set_pulse(int gpio, int pulse_us, char *output, size_t output_size)
{
    if (!gpio_policy_pin_is_allowed(gpio)) {
        gpio_policy_pin_forbidden_hint(gpio, output, output_size);
        if (output[0] == '\0')
            snprintf(output, output_size, "Error: GPIO %d is not in allowed list", gpio);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_timer();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: PWM timer init failed");
        return err;
    }

    pwm_slot_t *slot = find_slot_by_gpio(gpio);
    if (!slot) {
        slot = alloc_slot(gpio);
        if (!slot) {
            snprintf(output, output_size,
                     "Error: max %d PWM channels reached, release one first",
                     MAX_PWM_CHANNELS);
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
            return err;
        }
    }

    int duty = pulse_us_to_duty(pulse_us);
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, slot->channel, duty);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set duty on GPIO %d", gpio);
        return err;
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, slot->channel);

    return ESP_OK;
}

/* --- RC config loading --- */

static void rc_load_defaults(void)
{
    s_rc.steer_gpio        = RC_STEER_GPIO_DEF;
    s_rc.steer_center_us   = RC_STEER_CENTER_US;
    s_rc.steer_min_us      = RC_STEER_MIN_US;
    s_rc.steer_max_us      = RC_STEER_MAX_US;
    s_rc.throttle_gpio     = RC_THROTTLE_GPIO_DEF;
    s_rc.throttle_neutral_us  = RC_THROTTLE_NEUTRAL;
    s_rc.throttle_forward_us  = RC_THROTTLE_FWD_US;
    s_rc.throttle_reverse_us  = RC_THROTTLE_REV_US;
    s_rc.steer_reversed      = false;
    s_rc.throttle_reversed   = false;
    s_rc.loaded = false;
}

static void rc_load_config(void)
{
    rc_load_defaults();

    FILE *f = fopen(RC_CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No rc.json found, using defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 2048) {
        fclose(f);
        return;
    }

    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    buf[fread(buf, 1, len, f)] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;

    cJSON *v;
    v = cJSON_GetObjectItem(root, "steer_gpio");
    if (cJSON_IsNumber(v)) s_rc.steer_gpio = v->valueint;
    v = cJSON_GetObjectItem(root, "steer_center_us");
    if (cJSON_IsNumber(v)) s_rc.steer_center_us = v->valueint;
    v = cJSON_GetObjectItem(root, "steer_min_us");
    if (cJSON_IsNumber(v)) s_rc.steer_min_us = v->valueint;
    v = cJSON_GetObjectItem(root, "steer_max_us");
    if (cJSON_IsNumber(v)) s_rc.steer_max_us = v->valueint;
    v = cJSON_GetObjectItem(root, "throttle_gpio");
    if (cJSON_IsNumber(v)) s_rc.throttle_gpio = v->valueint;
    v = cJSON_GetObjectItem(root, "throttle_neutral_us");
    if (cJSON_IsNumber(v)) s_rc.throttle_neutral_us = v->valueint;
    v = cJSON_GetObjectItem(root, "throttle_forward_us");
    if (cJSON_IsNumber(v)) s_rc.throttle_forward_us = v->valueint;
    v = cJSON_GetObjectItem(root, "throttle_reverse_us");
    if (cJSON_IsNumber(v)) s_rc.throttle_reverse_us = v->valueint;
    v = cJSON_GetObjectItem(root, "steer_reversed");
    if (v) s_rc.steer_reversed = cJSON_IsTrue(v);
    v = cJSON_GetObjectItem(root, "throttle_reversed");
    if (v) s_rc.throttle_reversed = cJSON_IsTrue(v);

    cJSON_Delete(root);
    s_rc.loaded = true;
    ESP_LOGI(TAG, "RC config loaded: steer=GPIO%d (%d/%d/%d us), throttle=GPIO%d (%d/%d/%d us)",
             s_rc.steer_gpio, s_rc.steer_min_us, s_rc.steer_center_us, s_rc.steer_max_us,
             s_rc.throttle_gpio, s_rc.throttle_reverse_us, s_rc.throttle_neutral_us, s_rc.throttle_forward_us);
}

/* --- Init --- */

esp_err_t tool_pwm_init(void)
{
    for (int i = 0; i < MAX_PWM_CHANNELS; i++) {
        s_slots[i].gpio = -1;
    }
    s_next_channel = 0;
    s_timer_configured = false;
    rc_load_config();
    ESP_LOGI(TAG, "PWM tool initialized (max %d channels)", MAX_PWM_CHANNELS);
    return ESP_OK;
}

/* --- Tool: pwm_set --- */

esp_err_t tool_pwm_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *gpio_obj   = cJSON_GetObjectItem(root, "gpio");
    cJSON *pulse_obj  = cJSON_GetObjectItem(root, "pulse_us");

    if (!cJSON_IsNumber(gpio_obj)) {
        snprintf(output, output_size, "Error: 'gpio' required (integer pin number)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(pulse_obj)) {
        snprintf(output, output_size, "Error: 'pulse_us' required (pulse width in microseconds)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int gpio     = (int)gpio_obj->valuedouble;
    int pulse_us = (int)pulse_obj->valuedouble;

    if (pulse_us < 0 || pulse_us > PWM_PERIOD_US) {
        snprintf(output, output_size, "Error: pulse_us must be 0-%d, got %d", PWM_PERIOD_US, pulse_us);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = pwm_set_pulse(gpio, pulse_us, output, output_size);
    if (err == ESP_OK) {
        snprintf(output, output_size, "PWM on GPIO %d set to %d us", gpio, pulse_us);
        ESP_LOGI(TAG, "pwm_set: GPIO %d -> %d us", gpio, pulse_us);
    }

    cJSON_Delete(root);
    return err;
}

/* --- Tool: pwm_release --- */

esp_err_t tool_pwm_release_execute(const char *input_json, char *output, size_t output_size)
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

    pwm_slot_t *slot = find_slot_by_gpio(gpio);
    if (!slot) {
        snprintf(output, output_size, "No active PWM on GPIO %d", gpio);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, slot->channel, 0);
    slot->gpio = -1;

    snprintf(output, output_size, "PWM on GPIO %d released", gpio);
    ESP_LOGI(TAG, "pwm_release: GPIO %d released", gpio);

    cJSON_Delete(root);
    return ESP_OK;
}

/* --- Tool: rc_steer --- */

esp_err_t tool_rc_steer_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pct_obj = cJSON_GetObjectItem(root, "steer_pct");
    if (!cJSON_IsNumber(pct_obj)) {
        snprintf(output, output_size, "Error: 'steer_pct' required (-100 to 100)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int pct = (int)pct_obj->valuedouble;
    if (s_rc.steer_reversed) pct = -pct;
    if (pct < -100 || pct > 100) {
        snprintf(output, output_size, "Error: steer_pct must be -100 to 100, got %d", pct);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Map percentage to pulse width:
     * -100% -> steer_min_us, 0% -> steer_center_us, +100% -> steer_max_us */
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_min_us - s_rc.steer_center_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_max_us - s_rc.steer_center_us) * pct / 100);
    }

    esp_err_t err = pwm_set_pulse(s_rc.steer_gpio, pulse_us, output, output_size);
    if (err == ESP_OK) {
        const char *dir = pct < 0 ? "left" : (pct > 0 ? "right" : "center");
        snprintf(output, output_size, "Steer %s %d%% (GPIO %d, %d us)", dir, abs(pct), s_rc.steer_gpio, pulse_us);
        ESP_LOGI(TAG, "rc_steer: %d%% -> %d us on GPIO %d", pct, pulse_us, s_rc.steer_gpio);
    }

    cJSON_Delete(root);
    return err;
}

/* --- Tool: rc_throttle --- */

esp_err_t tool_rc_throttle_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pct_obj = cJSON_GetObjectItem(root, "throttle_pct");
    if (!cJSON_IsNumber(pct_obj)) {
        snprintf(output, output_size, "Error: 'throttle_pct' required (-100 to 100)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int pct = (int)pct_obj->valuedouble;
    if (s_rc.throttle_reversed) pct = -pct;
    if (pct < -100 || pct > 100) {
        snprintf(output, output_size, "Error: throttle_pct must be -100 to 100, got %d", pct);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Map percentage to pulse width:
     * -100% -> reverse_us, 0% -> neutral_us, +100% -> forward_us */
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_reverse_us - s_rc.throttle_neutral_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_forward_us - s_rc.throttle_neutral_us) * pct / 100);
    }

    esp_err_t err = pwm_set_pulse(s_rc.throttle_gpio, pulse_us, output, output_size);
    if (err == ESP_OK) {
        const char *dir = pct < 0 ? "reverse" : (pct > 0 ? "forward" : "neutral");
        snprintf(output, output_size, "Throttle %s %d%% (GPIO %d, %d us)", dir, abs(pct), s_rc.throttle_gpio, pulse_us);
        ESP_LOGI(TAG, "rc_throttle: %d%% -> %d us on GPIO %d", pct, pulse_us, s_rc.throttle_gpio);
    }

    cJSON_Delete(root);
    return err;
}

/* --- Internal nav API (bypasses JSON, reuses existing PWM + rc_config) --- */

esp_err_t rc_nav_throttle(int pct)
{
    if (!s_rc.loaded) return ESP_ERR_INVALID_STATE;
    if (pct < -100) pct = -100;
    if (pct > 100) pct = 100;
    if (s_rc.throttle_reversed) pct = -pct;
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_reverse_us - s_rc.throttle_neutral_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.throttle_neutral_us +
            (int)((int64_t)(s_rc.throttle_forward_us - s_rc.throttle_neutral_us) * pct / 100);
    }
    char buf[64];
    return pwm_set_pulse(s_rc.throttle_gpio, pulse_us, buf, sizeof(buf));
}

esp_err_t rc_nav_steer(int pct)
{
    if (!s_rc.loaded) return ESP_ERR_INVALID_STATE;
    if (pct < -100) pct = -100;
    if (pct > 100) pct = 100;
    if (s_rc.steer_reversed) pct = -pct;
    int pulse_us;
    if (pct < 0) {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_min_us - s_rc.steer_center_us) * (-pct) / 100);
    } else {
        pulse_us = s_rc.steer_center_us +
            (int)((int64_t)(s_rc.steer_max_us - s_rc.steer_center_us) * pct / 100);
    }
    char buf[64];
    return pwm_set_pulse(s_rc.steer_gpio, pulse_us, buf, sizeof(buf));
}

bool rc_nav_steer_is_reversed(void)
{
    return s_rc.steer_reversed;
}
