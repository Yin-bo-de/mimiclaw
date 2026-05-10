#pragma once

#include "esp_err.h"
#include <stddef.h>

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} mimi_tool_t;

/**
 * Per-turn message origin: channel + chat_id of the inbound message currently
 * being processed.  Set by agent_loop before executing each tool call so that
 * nav_goto / nav_goto_waypoint can route escalate events back to the caller.
 * agent_loop is a single task — no mutex needed.
 */
typedef struct {
    char channel[16];
    char chat_id[96];
} tool_msg_origin_t;

void tool_registry_set_origin(const char *channel, const char *chat_id);
void tool_registry_get_origin(tool_msg_origin_t *out);

/**
 * Initialize tool registry and register all built-in tools.
 */
esp_err_t tool_registry_init(void);

/**
 * Get the pre-built tools JSON array string for the API request.
 * Returns NULL if no tools are registered.
 */
const char *tool_registry_get_tools_json(void);

/**
 * Execute a tool by name.
 *
 * @param name         Tool name (e.g. "web_search")
 * @param input_json   JSON string of tool input
 * @param output       Output buffer for tool result text
 * @param output_size  Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if tool unknown
 */
esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size);
