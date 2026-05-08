#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_pwm_init(void);

esp_err_t tool_pwm_set_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_pwm_release_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_rc_steer_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_rc_throttle_execute(const char *input_json, char *output, size_t output_size);
