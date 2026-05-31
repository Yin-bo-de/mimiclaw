#include "session_mgr.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "session";

/* ------------------------------------------------------------------ */
/*  RPC infrastructure: all SPIFFS I/O is delegated to a Core-0 worker */
/* ------------------------------------------------------------------ */

typedef enum {
    SESSION_OP_GET_HISTORY,
    SESSION_OP_APPEND,
    SESSION_OP_CLEAR,
    SESSION_OP_LIST,
} session_op_t;

typedef struct {
    session_op_t op;
    char chat_id[64];
    char role[16];
    const char *content;
    char *buf;
    size_t buf_size;
    int max_msgs;
    esp_err_t *result;
    TaskHandle_t caller;
} session_request_t;

static QueueHandle_t session_queue = NULL;
static bool session_initialized = false;

/* Forward declarations for the real SPIFFS-backed implementations */
static void session_path(const char *chat_id, char *buf, size_t size);
static esp_err_t session_append_impl(const char *chat_id, const char *role, const char *content);
static esp_err_t session_get_history_json_impl(const char *chat_id, char *buf, size_t size, int max_msgs);
static esp_err_t session_clear_impl(const char *chat_id);
static void session_list_impl(void);

/* Execute a request either directly (if already on Core 0) or via RPC */
static void session_rpc(session_request_t *req)
{
    req->caller = xTaskGetCurrentTaskHandle();

    /* If we are already on Core 0 (or the queue isn't up yet), run inline
     * to avoid deadlock and to skip the queue overhead. */
    if (xPortGetCoreID() == 0 || session_queue == NULL) {
        switch (req->op) {
            case SESSION_OP_APPEND:
                *req->result = session_append_impl(req->chat_id, req->role, req->content);
                break;
            case SESSION_OP_GET_HISTORY:
                *req->result = session_get_history_json_impl(req->chat_id, req->buf,
                                                              req->buf_size, req->max_msgs);
                break;
            case SESSION_OP_CLEAR:
                *req->result = session_clear_impl(req->chat_id);
                break;
            case SESSION_OP_LIST:
                session_list_impl();
                *req->result = ESP_OK;
                break;
        }
        return;
    }

    if (xQueueSend(session_queue, req, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Session queue full");
        *req->result = ESP_FAIL;
        return;
    }

    /* Block until the Core-0 worker finishes the SPIFFS operation */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

/* Core-0 worker: the only task that ever touches SPIFFS session files */
static void session_worker_task(void *arg)
{
    (void)arg;
    session_request_t req;

    while (1) {
        if (xQueueReceive(session_queue, &req, portMAX_DELAY) == pdTRUE) {
            switch (req.op) {
                case SESSION_OP_APPEND:
                    *req.result = session_append_impl(req.chat_id, req.role, req.content);
                    break;
                case SESSION_OP_GET_HISTORY:
                    *req.result = session_get_history_json_impl(req.chat_id, req.buf,
                                                                 req.buf_size, req.max_msgs);
                    break;
                case SESSION_OP_CLEAR:
                    *req.result = session_clear_impl(req.chat_id);
                    break;
                case SESSION_OP_LIST:
                    session_list_impl();
                    *req.result = ESP_OK;
                    break;
            }
            if (req.caller) {
                xTaskNotifyGive(req.caller);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public API — thin RPC wrappers                                    */
/* ------------------------------------------------------------------ */

esp_err_t session_mgr_init(void)
{
    if (session_initialized) {
        return ESP_OK;
    }

    session_queue = xQueueCreate(4, sizeof(session_request_t));
    if (!session_queue) {
        ESP_LOGE(TAG, "Failed to create session queue");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        session_worker_task,
        "session_worker",
        4096,
        NULL,
        5,
        NULL,
        0);                     /* Core 0 — the I/O core */

    if (ret != pdPASS) {
        vQueueDelete(session_queue);
        session_queue = NULL;
        return ESP_FAIL;
    }

    session_initialized = true;
    ESP_LOGI(TAG, "Session manager initialized at %s", MIMI_SPIFFS_SESSION_DIR);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    esp_err_t result;
    session_request_t req = {
        .op = SESSION_OP_APPEND,
        .content = content,
        .result = &result,
    };
    strncpy(req.chat_id, chat_id, sizeof(req.chat_id) - 1);
    strncpy(req.role, role, sizeof(req.role) - 1);
    session_rpc(&req);
    return result;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    esp_err_t result;
    session_request_t req = {
        .op = SESSION_OP_GET_HISTORY,
        .buf = buf,
        .buf_size = size,
        .max_msgs = max_msgs,
        .result = &result,
    };
    strncpy(req.chat_id, chat_id, sizeof(req.chat_id) - 1);
    session_rpc(&req);
    return result;
}

esp_err_t session_clear(const char *chat_id)
{
    esp_err_t result;
    session_request_t req = {
        .op = SESSION_OP_CLEAR,
        .result = &result,
    };
    strncpy(req.chat_id, chat_id, sizeof(req.chat_id) - 1);
    session_rpc(&req);
    return result;
}

void session_list(void)
{
    esp_err_t result;
    session_request_t req = {
        .op = SESSION_OP_LIST,
        .result = &result,
    };
    session_rpc(&req);
}

/* ------------------------------------------------------------------ */
/*  SPIFFS-backed implementations (only ever run on Core 0)          */
/* ------------------------------------------------------------------ */

static void session_path(const char *chat_id, char *buf, size_t size)
{
    snprintf(buf, size, "%s/tg_%s.jsonl", MIMI_SPIFFS_SESSION_DIR, chat_id);
}

static esp_err_t session_append_impl(const char *chat_id, const char *role, const char *content)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

static esp_err_t session_get_history_json_impl(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No history yet */
        snprintf(buf, size, "[]");
        return ESP_OK;
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[MIMI_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (role && content) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

static esp_err_t session_clear_impl(const char *chat_id)
{
    char path[64];
    session_path(chat_id, path, sizeof(path));

    if (remove(path) == 0) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static void session_list_impl(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_SESSION_DIR);
    if (!dir) {
        /* SPIFFS is flat, so list all files matching pattern */
        dir = opendir(MIMI_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "tg_") && strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}
