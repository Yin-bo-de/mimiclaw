#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_sensors_init(void);

/* CLI test tools */
esp_err_t tool_ultrasonic_test_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_imu_test_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_gps_test_execute(const char *input_json, char *output, size_t output_size);

/* LLM-facing sensor read tools (JSON output, no looping) */
esp_err_t tool_read_distance_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_read_imu_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_read_gps_execute(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif
