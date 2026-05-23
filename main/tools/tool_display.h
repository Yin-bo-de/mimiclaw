#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize display tools.
 */
esp_err_t tool_display_init(void);

/**
 * Set dashboard weather city.
 * Input JSON: {"city":"Tokyo"}
 */
esp_err_t tool_display_set_weather_city_execute(const char *input_json, char *output, size_t output_size);

/**
 * Set dashboard weather data. Summary is required; city is optional.
 * Input JSON: {"summary":"8C cloudy","city":"Tokyo"?,"updated_epoch":1770000000?}
 */
esp_err_t tool_display_set_weather_execute(const char *input_json, char *output, size_t output_size);

/**
 * Replace dashboard todos.
 * Input JSON: {"todos":["item 1","item 2"],"updated_epoch":1770000000?}
 */
esp_err_t tool_display_set_todos_execute(const char *input_json, char *output, size_t output_size);

/**
 * Return current dashboard state as JSON.
 * Input JSON: {}
 */
esp_err_t tool_display_get_state_execute(const char *input_json, char *output, size_t output_size);

/**
 * Request or perform dashboard refresh.
 * Input JSON: {"now":true?}
 */
esp_err_t tool_display_refresh_execute(const char *input_json, char *output, size_t output_size);
