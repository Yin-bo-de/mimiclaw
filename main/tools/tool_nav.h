#pragma once

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize nav tool subsystem (nav_situation + nav_config + nav_waypoints).
 * Called from tool_registry_init().
 */
esp_err_t tool_nav_init(void);

/* Phase 4: waypoint management + status */
esp_err_t tool_nav_save_waypoint_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_list_waypoints_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_delete_waypoint_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_status_execute(const char *input_json, char *output, size_t output_size);

/* Phase 6: autonomous navigation control */
esp_err_t tool_nav_goto_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_goto_waypoint_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_pause_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_resume_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_abort_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_nav_manual_step_execute(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif
