#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_pwm_init(void);

esp_err_t tool_pwm_set_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_pwm_release_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_rc_steer_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_rc_throttle_execute(const char *input_json, char *output, size_t output_size);

/* Internal API for nav layer — bypass JSON overhead */
esp_err_t rc_nav_throttle(int throttle_pct);  /* -100..100 */
esp_err_t rc_nav_steer(int steer_pct);        /* -100..100 */
