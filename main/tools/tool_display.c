#include "tools/tool_display.h"

#include "display/display_service.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tool_display";

static void copy_trimmed_string(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    dest[0] = '\0';
    if (!src) {
        return;
    }

    while (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n') {
        src++;
    }

    size_t len = strlen(src);
    while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t' ||
                       src[len - 1] == '\r' || src[len - 1] == '\n')) {
        len--;
    }

    if (len >= dest_size) {
        len = dest_size - 1;
        while (len > 0 && (((unsigned char)src[len] & 0xC0) == 0x80)) {
            len--;
        }
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

static cJSON *parse_json_or_error(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        snprintf(output, output_size, "Error: invalid JSON object input");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

esp_err_t tool_display_init(void)
{
    ESP_LOGI(TAG, "display tools initialized");
    return ESP_OK;
}

esp_err_t tool_display_set_weather_city_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_json_or_error(input_json, output, output_size);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    char city[MIMI_DISPLAY_WEATHER_CITY_LEN];
    copy_trimmed_string(city, sizeof(city), cJSON_GetStringValue(cJSON_GetObjectItem(root, "city")));
    cJSON_Delete(root);

    if (city[0] == '\0') {
        snprintf(output, output_size, "Error: 'city' required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = display_service_set_weather_city(city);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set weather city (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "OK: dashboard weather city set to %s", city);
    return ESP_OK;
}

esp_err_t tool_display_set_weather_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_json_or_error(input_json, output, output_size);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    char city[MIMI_DISPLAY_WEATHER_CITY_LEN];
    char summary[MIMI_DISPLAY_WEATHER_SUMMARY_LEN];
    copy_trimmed_string(city, sizeof(city), cJSON_GetStringValue(cJSON_GetObjectItem(root, "city")));
    copy_trimmed_string(summary, sizeof(summary), cJSON_GetStringValue(cJSON_GetObjectItem(root, "summary")));

    cJSON *updated = cJSON_GetObjectItem(root, "updated_epoch");
    int64_t updated_epoch = cJSON_IsNumber(updated) ? (int64_t)updated->valuedouble : 0;
    cJSON_Delete(root);

    if (summary[0] == '\0') {
        snprintf(output, output_size, "Error: 'summary' required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = display_service_set_weather(city[0] ? city : NULL, summary, updated_epoch);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set weather (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "OK: dashboard weather saved%s%s",
             city[0] ? " for " : "", city[0] ? city : "");
    return ESP_OK;
}

esp_err_t tool_display_set_todos_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_json_or_error(input_json, output, output_size);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *todos_json = cJSON_GetObjectItem(root, "todos");
    if (!cJSON_IsArray(todos_json)) {
        snprintf(output, output_size, "Error: 'todos' must be an array of strings");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    char todos_storage[MIMI_DISPLAY_MAX_TODOS][MIMI_DISPLAY_TODO_LEN];
    const char *todos[MIMI_DISPLAY_MAX_TODOS];
    size_t todo_count = 0;
    int input_count = cJSON_GetArraySize(todos_json);

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, todos_json) {
        if (todo_count >= MIMI_DISPLAY_MAX_TODOS) {
            break;
        }
        const char *value = cJSON_GetStringValue(item);
        if (!value) {
            snprintf(output, output_size, "Error: all todo items must be strings");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        copy_trimmed_string(todos_storage[todo_count], MIMI_DISPLAY_TODO_LEN, value);
        if (todos_storage[todo_count][0] == '\0') {
            continue;
        }
        todos[todo_count] = todos_storage[todo_count];
        todo_count++;
    }

    cJSON *updated = cJSON_GetObjectItem(root, "updated_epoch");
    int64_t updated_epoch = cJSON_IsNumber(updated) ? (int64_t)updated->valuedouble : 0;
    cJSON_Delete(root);

    esp_err_t err = display_service_set_todos(todos, todo_count, updated_epoch);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set todos (%s)", esp_err_to_name(err));
        return err;
    }

    if (input_count > MIMI_DISPLAY_MAX_TODOS) {
        snprintf(output, output_size, "OK: saved %d todos (capped from %d)", (int)todo_count, input_count);
    } else {
        snprintf(output, output_size, "OK: saved %d todos", (int)todo_count);
    }
    return ESP_OK;
}

esp_err_t tool_display_set_quote_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = parse_json_or_error(input_json, output, output_size);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    char quote[MIMI_DISPLAY_QUOTE_LEN];
    copy_trimmed_string(quote, sizeof(quote), cJSON_GetStringValue(cJSON_GetObjectItem(root, "quote")));
    cJSON_Delete(root);

    if (quote[0] == '\0') {
        snprintf(output, output_size, "Error: 'quote' required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = display_service_set_quote(quote, 0);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set quote (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "OK: dashboard quote saved");
    return ESP_OK;
}

esp_err_t tool_display_get_state_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    return display_service_get_state_json(output, output_size);
}

esp_err_t tool_display_refresh_execute(const char *input_json, char *output, size_t output_size)
{
    bool now = false;
    cJSON *root = parse_json_or_error(input_json, output, output_size);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *now_json = cJSON_GetObjectItem(root, "now");
    now = cJSON_IsTrue(now_json);
    cJSON_Delete(root);

    esp_err_t err;
    if (now) {
        err = display_service_refresh_now();
        if (err == ESP_ERR_INVALID_STATE) {
            snprintf(output, output_size, "OK: refresh skipped; display hardware unavailable, state is saved");
            return ESP_OK;
        }
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: refresh failed (%s)", esp_err_to_name(err));
            return err;
        }
        snprintf(output, output_size, "OK: dashboard refreshed now");
        return ESP_OK;
    }

    err = display_service_request_refresh();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to request refresh (%s)", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "OK: dashboard refresh requested");
    return ESP_OK;
}
