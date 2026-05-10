#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_sensors_init(void);

esp_err_t tool_ultrasonic_test_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_imu_test_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_gps_test_execute(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif
