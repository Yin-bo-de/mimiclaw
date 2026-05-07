#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize script engine — ensure script directory exists.
 */
esp_err_t tool_script_init(void);

/**
 * Create or overwrite a named script.
 * Input JSON: {"name": <string>, "steps": [{"tool": <string>, "input": <object>, "delay_ms": <int?>}, ...]}
 */
esp_err_t tool_script_create_execute(const char *input_json, char *output, size_t output_size);

/**
 * Execute a named script step by step.
 * Input JSON: {"name": <string>}
 */
esp_err_t tool_script_run_execute(const char *input_json, char *output, size_t output_size);

/**
 * List all stored scripts with step counts.
 * Input JSON: {}
 */
esp_err_t tool_script_list_execute(const char *input_json, char *output, size_t output_size);

/**
 * Delete a named script.
 * Input JSON: {"name": <string>}
 */
esp_err_t tool_script_remove_execute(const char *input_json, char *output, size_t output_size);
