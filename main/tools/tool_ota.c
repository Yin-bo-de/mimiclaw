#include "tools/tool_ota.h"
#include "ota/ota_manager.h"

#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "tool_ota";

typedef struct {
    char url[512];
    esp_err_t result;
    SemaphoreHandle_t done;
} ota_task_ctx_t;

static void ota_task(void *arg)
{
    ota_task_ctx_t *ctx = (ota_task_ctx_t *)arg;
    ctx->result = ota_update_from_url(ctx->url);
    /* Only reaches here on failure; success triggers esp_restart() */
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

esp_err_t tool_ota_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_obj = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(url_obj) || !url_obj->valuestring || !url_obj->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'url' is required");
        return ESP_ERR_INVALID_ARG;
    }

    ota_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    strncpy(ctx->url, url_obj->valuestring, sizeof(ctx->url) - 1);
    cJSON_Delete(root);

    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Starting OTA task for: %s", ctx->url);

    if (xTaskCreate(ota_task, "ota_agent", 8192, ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        snprintf(output, output_size, "Error: failed to create OTA task");
        return ESP_FAIL;
    }

    /* Wait up to 130s. On success the device reboots and we never return here. */
    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(130000)) != pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        snprintf(output, output_size, "Error: OTA timed out");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ctx->result;
    vSemaphoreDelete(ctx->done);
    free(ctx);

    snprintf(output, output_size, "OTA failed: %s", esp_err_to_name(err));
    return err;
}
