#include "tools/tool_script.h"
#include "tools/tool_registry.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const char *TAG = "tool_script";

#define SCRIPT_DIR     MIMI_SCRIPT_DIR
#define SCRIPT_MAX_STEPS   64
#define SCRIPT_MAX_NAME    32
#define SCRIPT_OUTPUT_BUF  512

static esp_err_t ensure_script_dir(void)
{
    struct stat st;
    if (stat(SCRIPT_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    if (mkdir(SCRIPT_DIR, 0775) != 0) {
        ESP_LOGE(TAG, "Failed to create script dir: %s", SCRIPT_DIR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Created script dir: %s", SCRIPT_DIR);
    return ESP_OK;
}

static void script_path(const char *name, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s/%s.json", SCRIPT_DIR, name);
}

esp_err_t tool_script_init(void)
{
    esp_err_t err = ensure_script_dir();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Script engine initialized (dir: %s)", SCRIPT_DIR);
    return ESP_OK;
}

esp_err_t tool_script_create_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    cJSON *steps_obj = cJSON_GetObjectItem(root, "steps");

    if (!cJSON_IsString(name_obj) || name_obj->valuestring[0] == '\0') {
        snprintf(output, output_size, "Error: 'name' required (non-empty string)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsArray(steps_obj) || cJSON_GetArraySize(steps_obj) == 0) {
        snprintf(output, output_size, "Error: 'steps' required (non-empty array)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = name_obj->valuestring;
    if (strlen(name) > SCRIPT_MAX_NAME) {
        snprintf(output, output_size, "Error: script name too long (max %d chars)", SCRIPT_MAX_NAME);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Script name: only alphanumeric and underscore */
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            snprintf(output, output_size,
                     "Error: script name must contain only letters, digits, and underscores");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Validate each step */
    int step_count = cJSON_GetArraySize(steps_obj);
    if (step_count > SCRIPT_MAX_STEPS) {
        snprintf(output, output_size, "Error: too many steps (max %d)", SCRIPT_MAX_STEPS);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < step_count; i++) {
        cJSON *step = cJSON_GetArrayItem(steps_obj, i);
        cJSON *tool_obj = cJSON_GetObjectItem(step, "tool");
        cJSON *input_obj = cJSON_GetObjectItem(step, "input");
        if (!cJSON_IsString(tool_obj) || !cJSON_IsObject(input_obj)) {
            snprintf(output, output_size, "Error: step %d requires 'tool' (string) and 'input' (object)", i + 1);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Write to SPIFFS */
    char path[64];
    script_path(name, path, sizeof(path));

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        snprintf(output, output_size, "Error: failed to serialize script");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(output, output_size, "Error: failed to write script file");
        cJSON_free(json_str);
        return ESP_FAIL;
    }
    fputs(json_str, f);
    fclose(f);
    cJSON_free(json_str);

    snprintf(output, output_size, "Script '%s' created with %d steps", name, step_count);
    ESP_LOGI(TAG, "Script created: %s (%d steps)", name, step_count);
    return ESP_OK;
}

esp_err_t tool_script_run_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name_obj) || name_obj->valuestring[0] == '\0') {
        snprintf(output, output_size, "Error: 'name' required (script name)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = name_obj->valuestring;
    cJSON_Delete(root);

    /* Read script file */
    char path[64];
    script_path(name, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: script '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        fclose(f);
        snprintf(output, output_size, "Error: script file invalid or too large");
        return ESP_FAIL;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *script = cJSON_Parse(buf);
    free(buf);
    if (!script) {
        snprintf(output, output_size, "Error: failed to parse script file");
        return ESP_FAIL;
    }

    cJSON *steps_obj = cJSON_GetObjectItem(script, "steps");
    if (!cJSON_IsArray(steps_obj)) {
        cJSON_Delete(script);
        snprintf(output, output_size, "Error: script has no 'steps' array");
        return ESP_FAIL;
    }

    int total = cJSON_GetArraySize(steps_obj);
    int ok_count = 0;
    int fail_count = 0;
    char step_output[SCRIPT_OUTPUT_BUF];

    for (int i = 0; i < total; i++) {
        cJSON *step = cJSON_GetArrayItem(steps_obj, i);
        cJSON *tool_obj = cJSON_GetObjectItem(step, "tool");
        cJSON *input_obj = cJSON_GetObjectItem(step, "input");

        if (!cJSON_IsString(tool_obj) || !cJSON_IsObject(input_obj)) {
            ESP_LOGW(TAG, "Step %d: invalid format, skipping", i + 1);
            fail_count++;
            continue;
        }

        char *input_str = cJSON_PrintUnformatted(input_obj);
        esp_err_t err = tool_registry_execute(tool_obj->valuestring, input_str,
                                               step_output, sizeof(step_output));
        cJSON_free(input_str);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Step %d (%s) failed: %s", i + 1, tool_obj->valuestring, step_output);
            fail_count++;
        } else {
            ESP_LOGI(TAG, "Step %d (%s): %s", i + 1, tool_obj->valuestring, step_output);
            ok_count++;
        }

        /* Optional delay between steps */
        cJSON *delay_obj = cJSON_GetObjectItem(step, "delay_ms");
        if (cJSON_IsNumber(delay_obj) && delay_obj->valueint > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_obj->valueint));
        }
    }

    cJSON_Delete(script);

    snprintf(output, output_size,
             "Script '%s' completed: %d/%d steps ok",
             name, ok_count, total);
    ESP_LOGI(TAG, "Script '%s' done: %d ok, %d fail of %d", name, ok_count, fail_count, total);

    return (fail_count == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t tool_script_list_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    DIR *dir = opendir(SCRIPT_DIR);
    if (!dir) {
        snprintf(output, output_size, "No scripts found");
        return ESP_OK;
    }

    char *cursor = output;
    size_t remaining = output_size;
    int count = 0;
    int written;

    written = snprintf(cursor, remaining, "Scripts:\n");
    if (written < 0 || (size_t)written >= remaining) goto done;
    cursor += (size_t)written;
    remaining -= (size_t)written;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *fname = ent->d_name;
        size_t len = strlen(fname);
        if (len < 6 || strcmp(fname + len - 5, ".json") != 0) continue;

        /* Strip .json suffix to get script name */
        char name[SCRIPT_MAX_NAME + 1];
        size_t nlen = len - 5;
        if (nlen > SCRIPT_MAX_NAME) nlen = SCRIPT_MAX_NAME;
        memcpy(name, fname, nlen);
        name[nlen] = '\0';

        /* Count steps */
        char path[64];
        script_path(name, path, sizeof(path));
        FILE *f = fopen(path, "r");
        int steps = 0;
        if (f) {
            fseek(f, 0, SEEK_END);
            long fsize = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *buf = malloc(fsize + 1);
            if (buf) {
                fread(buf, 1, fsize, f);
                buf[fsize] = '\0';
                cJSON *script = cJSON_Parse(buf);
                if (script) {
                    cJSON *steps_arr = cJSON_GetObjectItem(script, "steps");
                    if (cJSON_IsArray(steps_arr)) steps = cJSON_GetArraySize(steps_arr);
                    cJSON_Delete(script);
                }
                free(buf);
            }
            fclose(f);
        }

        written = snprintf(cursor, remaining, "- %s (%d steps)\n", name, steps);
        if (written < 0 || (size_t)written >= remaining) break;
        cursor += (size_t)written;
        remaining -= (size_t)written;
        count++;
    }

    closedir(dir);

done:
    if (count == 0) {
        snprintf(output, output_size, "No scripts found");
    }
    return ESP_OK;
}

esp_err_t tool_script_remove_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *name_obj = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(name_obj) || name_obj->valuestring[0] == '\0') {
        snprintf(output, output_size, "Error: 'name' required (script name)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const char *name = name_obj->valuestring;
    cJSON_Delete(root);

    char path[64];
    script_path(name, path, sizeof(path));

    if (remove(path) != 0) {
        snprintf(output, output_size, "Error: script '%s' not found", name);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(output, output_size, "Script '%s' removed", name);
    ESP_LOGI(TAG, "Script removed: %s", name);
    return ESP_OK;
}
