#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_rule_add_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_rule_remove_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_rule_list_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_rule_enable_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_rule_disable_execute(const char *input_json, char *output, size_t output_size);
