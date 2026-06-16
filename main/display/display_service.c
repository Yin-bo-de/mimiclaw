#ifdef MIMI_DISPLAY_SERVICE_WEATHER_PARSE_TEST
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#else
#include "display/display_service.h"
#endif

#include "util/utf8.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef MIMI_DISPLAY_SERVICE_WEATHER_PARSE_TEST
#define MIMI_WEATHER_PARSE_VISIBILITY
#else
#define MIMI_WEATHER_PARSE_VISIBILITY static __attribute__((unused))
#endif

static void copy_trimmed_weather_line(const char *line, size_t len, char *output, size_t output_size)
{
    while (len > 0 && (isspace((unsigned char)*line))) {
        line++;
        len--;
    }
    while (len > 0 && (isspace((unsigned char)line[len - 1]))) {
        len--;
    }

    size_t copy_len = len;
    if (copy_len >= output_size) {
        copy_len = output_size - 1;
    }
    memcpy(output, line, copy_len);
    output[copy_len] = '\0';
    mimi_trim_incomplete_utf8_tail(output);
}

MIMI_WEATHER_PARSE_VISIBILITY esp_err_t parse_wttr_weather_response(const char *response,
                                                                    char *city,
                                                                    size_t city_size,
                                                                    char *summary,
                                                                    size_t summary_size)
{
    if (!response || !city || city_size == 0 || !summary || summary_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    city[0] = '\0';
    summary[0] = '\0';

    const char *cursor = response;
    size_t parsed_count = 0;
    while (*cursor != '\0' && parsed_count < 2) {
        size_t line_len = strcspn(cursor, "\r\n");
        const char *line = cursor;
        size_t trimmed_len = line_len;
        while (trimmed_len > 0 && (isspace((unsigned char)*line))) {
            line++;
            trimmed_len--;
        }
        while (trimmed_len > 0 && (line[trimmed_len - 1] == ' ' || line[trimmed_len - 1] == '\t')) {
            trimmed_len--;
        }

        if (trimmed_len > 0) {
            if (parsed_count == 0) {
                copy_trimmed_weather_line(cursor, line_len, city, city_size);
            } else {
                copy_trimmed_weather_line(cursor, line_len, summary, summary_size);
            }
            parsed_count++;
        }

        cursor += line_len;
        while (*cursor == '\r' || *cursor == '\n') {
            cursor++;
        }
    }

    return parsed_count > 0 ? ESP_OK : ESP_FAIL;
}

#ifdef MIMI_DISPLAY_SERVICE_WEATHER_PARSE_TEST
#undef MIMI_WEATHER_PARSE_VISIBILITY
#else

#include "display/display_lvgl.h"
#include "display/epaper_waveshare_2in9_v2.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "cron/cron_service.h"
#include "tools/tool_get_time.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "display_service";

#define DISPLAY_EVENT_REFRESH BIT0
#define DISPLAY_DIRTY_HEADER  BIT0
#define DISPLAY_DIRTY_WEATHER BIT1
#define DISPLAY_DIRTY_TODOS   BIT2
#define DISPLAY_DIRTY_QUOTE   BIT3
#define DISPLAY_DIRTY_FULL    BIT4
#define DISPLAY_BOOT_UPDATE_STACK 8192
#define DISPLAY_BOOT_UPDATE_PRIO  3
#define DISPLAY_STATE_TMP_FILE MIMI_DISPLAY_STATE_FILE ".tmp"
#define DISPLAY_WEATHER_HTTP_BUF_SIZE 512
#define DISPLAY_WEATHER_HTTP_TIMEOUT_MS 10000
#define DISPLAY_WEATHER_WTTR_URL "http://wttr.in/?format=%l%0A%C+%t&lang=zh"

typedef struct {
    char ip_address[16];
} display_boot_update_ctx_t;

typedef struct {
    char data[DISPLAY_WEATHER_HTTP_BUF_SIZE];
    size_t len;
    bool truncated;
} display_weather_http_buf_t;

static mimi_display_state_t s_state;
static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_display_events;
static TaskHandle_t s_display_task;
static bool s_initialized;
static bool s_driver_attempted;
static uint32_t s_dirty_regions;
static uint8_t s_framebuffer[MIMI_DISPLAY_FB_BYTES];

static int64_t current_epoch_or_zero(void)
{
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
}

static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
    display_weather_http_buf_t *buf = (display_weather_http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data && evt->data_len > 0) {
        size_t available = sizeof(buf->data) - buf->len - 1;
        size_t copy_len = (size_t)evt->data_len;
        if (copy_len > available) {
            copy_len = available;
            buf->truncated = true;
        }
        if (copy_len > 0) {
            memcpy(buf->data + buf->len, evt->data, copy_len);
            buf->len += copy_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t fetch_weather_wttr(char *city, size_t city_size, char *summary, size_t summary_size)
{
    if (!city || city_size == 0 || !summary || summary_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    city[0] = '\0';
    summary[0] = '\0';

    display_weather_http_buf_t response = {0};
    esp_http_client_config_t config = {
        .url = DISPLAY_WEATHER_WTTR_URL,
        .timeout_ms = DISPLAY_WEATHER_HTTP_TIMEOUT_MS,
        .event_handler = weather_http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "wttr weather HTTP status=%d", status);
            err = ESP_FAIL;
        } else if (response.truncated) {
            ESP_LOGW(TAG, "wttr weather response truncated at %u bytes", (unsigned)response.len);
            err = ESP_FAIL;
        } else {
            err = parse_wttr_weather_response(response.data, city, city_size, summary, summary_size);
            if (err == ESP_OK && (city[0] == '\0' || summary[0] == '\0')) {
                ESP_LOGW(TAG, "wttr weather response missing city or summary");
                err = ESP_FAIL;
            }
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t lock_state(void)
{
    if (!s_state_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    return xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void unlock_state(void)
{
    if (s_state_mutex) {
        xSemaphoreGive(s_state_mutex);
    }
}

static void mark_dirty_region(uint32_t region_bits)
{
    if (lock_state() == ESP_OK) {
        if ((region_bits & DISPLAY_DIRTY_FULL) != 0) {
            s_dirty_regions = DISPLAY_DIRTY_FULL;
        } else if ((s_dirty_regions & DISPLAY_DIRTY_FULL) == 0) {
            s_dirty_regions |= region_bits;
        }
        unlock_state();
    }
}

static uint32_t take_dirty_regions(void)
{
    uint32_t regions = 0;
    if (lock_state() == ESP_OK) {
        regions = s_dirty_regions;
        s_dirty_regions = 0;
        unlock_state();
    }
    return regions;
}

static esp_err_t save_state_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "weather_city", s_state.weather_city);
    cJSON_AddStringToObject(root, "weather_summary", s_state.weather_summary);
    cJSON_AddNumberToObject(root, "weather_updated_epoch", (double)s_state.weather_updated_epoch);

    cJSON *todos = cJSON_CreateArray();
    if (!todos) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s_state.todo_count && i < MIMI_DISPLAY_MAX_TODOS; i++) {
        cJSON_AddItemToArray(todos, cJSON_CreateString(s_state.todos[i]));
    }
    cJSON_AddItemToObject(root, "todos", todos);
    cJSON_AddNumberToObject(root, "todos_updated_epoch", (double)s_state.todos_updated_epoch);
    cJSON_AddStringToObject(root, "quote", s_state.quote);
    cJSON_AddNumberToObject(root, "quote_updated_epoch", (double)s_state.quote_updated_epoch);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    remove(DISPLAY_STATE_TMP_FILE);
    FILE *file = fopen(DISPLAY_STATE_TMP_FILE, "w");
    if (!file) {
        ESP_LOGW(TAG, "failed to open %s for write", DISPLAY_STATE_TMP_FILE);
        cJSON_free(json);
        return ESP_FAIL;
    }

    size_t len = strlen(json);
    size_t written = fwrite(json, 1, len, file);
    int close_result = fclose(file);
    cJSON_free(json);

    if (written != len || close_result != 0) {
        ESP_LOGW(TAG, "failed to write display state temp file (%d/%d, fclose=%d)",
                 (int)written, (int)len, close_result);
        remove(DISPLAY_STATE_TMP_FILE);
        return ESP_FAIL;
    }

    if (rename(DISPLAY_STATE_TMP_FILE, MIMI_DISPLAY_STATE_FILE) != 0) {
        remove(MIMI_DISPLAY_STATE_FILE);
        if (rename(DISPLAY_STATE_TMP_FILE, MIMI_DISPLAY_STATE_FILE) != 0) {
            ESP_LOGW(TAG, "failed to replace %s with %s", MIMI_DISPLAY_STATE_FILE, DISPLAY_STATE_TMP_FILE);
            remove(DISPLAY_STATE_TMP_FILE);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t load_state_locked(void)
{
    struct stat st;
    if (stat(MIMI_DISPLAY_STATE_FILE, &st) != 0 || st.st_size <= 0 || st.st_size > 2048) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE *file = fopen(MIMI_DISPLAY_STATE_FILE, "r");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    char json[2049];
    size_t read_len = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[read_len] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "display state JSON is invalid; using defaults");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *weather_city = cJSON_GetStringValue(cJSON_GetObjectItem(root, "weather_city"));
    const char *weather_summary = cJSON_GetStringValue(cJSON_GetObjectItem(root, "weather_summary"));
    cJSON *weather_epoch = cJSON_GetObjectItem(root, "weather_updated_epoch");
    cJSON *todos_epoch = cJSON_GetObjectItem(root, "todos_updated_epoch");

    mimi_copy_string_truncated_utf8(s_state.weather_city, sizeof(s_state.weather_city), weather_city);
    mimi_copy_string_truncated_utf8(s_state.weather_summary, sizeof(s_state.weather_summary), weather_summary);
    s_state.weather_updated_epoch = cJSON_IsNumber(weather_epoch) ? (int64_t)weather_epoch->valuedouble : 0;
    s_state.todos_updated_epoch = cJSON_IsNumber(todos_epoch) ? (int64_t)todos_epoch->valuedouble : 0;
    s_state.todo_count = 0;

    cJSON *todos = cJSON_GetObjectItem(root, "todos");
    if (cJSON_IsArray(todos)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, todos) {
            if (s_state.todo_count >= MIMI_DISPLAY_MAX_TODOS) {
                break;
            }
            const char *todo = cJSON_GetStringValue(item);
            if (!todo) {
                continue;
            }
            mimi_copy_string_truncated_utf8(s_state.todos[s_state.todo_count], MIMI_DISPLAY_TODO_LEN, todo);
            s_state.todo_count++;
        }
    }

    const char *quote = cJSON_GetStringValue(cJSON_GetObjectItem(root, "quote"));
    cJSON *quote_epoch = cJSON_GetObjectItem(root, "quote_updated_epoch");
    mimi_copy_string_truncated_utf8(s_state.quote, sizeof(s_state.quote), quote);
    s_state.quote_updated_epoch = cJSON_IsNumber(quote_epoch) ? (int64_t)quote_epoch->valuedouble : 0;

    cJSON_Delete(root);
    return ESP_OK;
}

static const char *weekday_zh(int weekday)
{
    static const char *names[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    if (weekday < 0 || weekday > 6) {
        return "";
    }
    return names[weekday];
}

static void format_local_datetime(char *date, size_t date_size,
                                  char *weekday, size_t weekday_size,
                                  char *time_text, size_t time_size)
{
    time_t now = time(NULL);
    struct tm local;

    if (now <= 0 || !localtime_r(&now, &local)) {
        if (date_size > 0) {
            date[0] = '\0';
        }
        if (weekday_size > 0) {
            weekday[0] = '\0';
        }
        snprintf(time_text, time_size, "Time syncing...");
        return;
    }

    strftime(date, date_size, "%Y-%m-%d", &local);
    snprintf(weekday, weekday_size, "%s", weekday_zh(local.tm_wday));
    strftime(time_text, time_size, "%H:%M", &local);
}

static esp_err_t build_dashboard_data(mimi_display_state_t *snapshot,
                                      char *date, size_t date_size,
                                      char *weekday, size_t weekday_size,
                                      char *time_text, size_t time_size,
                                      display_dashboard_data_t *data)
{
    esp_err_t err = display_service_get_state(snapshot);
    if (err != ESP_OK) {
        return err;
    }

    format_local_datetime(date, date_size, weekday, weekday_size, time_text, time_size);
    memset(data, 0, sizeof(*data));
    data->date = date;
    data->weekday = weekday;
    data->time = time_text;
    data->weather_city = snapshot->weather_city;
    data->weather_summary = snapshot->weather_summary;
    data->todo_count = snapshot->todo_count;
    for (size_t i = 0; i < snapshot->todo_count && i < MIMI_DISPLAY_MAX_TODOS; i++) {
        data->todos[i] = snapshot->todos[i];
    }
    data->quote = snapshot->quote;
    return ESP_OK;
}

static esp_err_t render_dashboard_regions(uint32_t dirty_regions)
{
#if MIMI_DISPLAY_ENABLED
    if (!s_state.display_available) {
        return ESP_ERR_INVALID_STATE;
    }

    mimi_display_state_t snapshot;
    char date[16];
    char weekday[8];
    char time_text[16];
    display_dashboard_data_t data;
    esp_err_t err = build_dashboard_data(&snapshot, date, sizeof(date), weekday, sizeof(weekday),
                                         time_text, sizeof(time_text), &data);
    if (err != ESP_OK) {
        return err;
    }

    (void)dirty_regions;
    esp_err_t render_err = display_lvgl_render_dashboard(&data);
    if (render_err != ESP_OK) {
        return render_err;
    }
    return epaper_waveshare_2in9_v2_display_frame(s_framebuffer, sizeof(s_framebuffer));
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t render_current_dashboard(void)
{
    return render_dashboard_regions(DISPLAY_DIRTY_FULL);
}

static void mark_display_available(bool available)
{
    if (lock_state() == ESP_OK) {
        s_state.display_available = available;
        unlock_state();
    }
}

static void display_task(void *arg)
{
    (void)arg;

#if MIMI_DISPLAY_ENABLED
    if (!s_driver_attempted) {
        s_driver_attempted = true;
        esp_err_t err = epaper_waveshare_2in9_v2_init();
        if (err == ESP_OK) {
            mark_display_available(true);
            ESP_LOGI(TAG, "display driver ready");
            esp_err_t lvgl_err = display_lvgl_init(s_framebuffer, sizeof(s_framebuffer));
            if (lvgl_err != ESP_OK) {
                mark_display_available(false);
                ESP_LOGW(TAG, "LVGL display unavailable: %s; dashboard persistence remains active", esp_err_to_name(lvgl_err));
                vTaskDelete(NULL);
            }
#if MIMI_DISPLAY_DIAGNOSTIC_BOOT_PATTERN
            esp_err_t diagnostic_err = epaper_waveshare_2in9_v2_test_pattern();
            if (diagnostic_err != ESP_OK) {
                ESP_LOGW(TAG, "diagnostic display pattern failed: %s", esp_err_to_name(diagnostic_err));
            } else {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
#endif
            esp_err_t render_err = render_current_dashboard();
            if (render_err != ESP_OK) {
                ESP_LOGW(TAG, "initial dashboard render failed: %s", esp_err_to_name(render_err));
            }
        } else {
            mark_display_available(false);
            ESP_LOGW(TAG, "display driver unavailable: %s; dashboard persistence remains active", esp_err_to_name(err));
        }
    }
#else
    mark_display_available(false);
    ESP_LOGI(TAG, "display disabled by config; dashboard persistence remains active");
#endif

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(s_display_events, DISPLAY_EVENT_REFRESH,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(MIMI_DISPLAY_REFRESH_MS));
        if ((bits & DISPLAY_EVENT_REFRESH) != 0) {
            ESP_LOGI(TAG, "dashboard refresh requested");
        }

        if (!display_service_is_display_available()) {
            continue;
        }

        uint32_t dirty_regions = take_dirty_regions();
        if (dirty_regions == 0) {
            if ((bits & DISPLAY_EVENT_REFRESH) != 0) {
                continue;
            }
            dirty_regions = DISPLAY_DIRTY_HEADER;
        }

        esp_err_t err = render_dashboard_regions(dirty_regions);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dashboard render failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t display_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state_mutex = xSemaphoreCreateMutex();
    s_display_events = xEventGroupCreate();
    if (!s_state_mutex || !s_display_events) {
        ESP_LOGE(TAG, "failed to create display service synchronization primitives");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }
    s_state.display_available = false;
    s_state.service_started = false;
    esp_err_t load_err = load_state_locked();
    unlock_state();

    if (load_err == ESP_OK) {
        ESP_LOGI(TAG, "loaded display state from %s", MIMI_DISPLAY_STATE_FILE);
    } else if (load_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "display state load failed: %s", esp_err_to_name(load_err));
    }

    s_initialized = true;
    ESP_LOGI(TAG, "display service initialized");
    return ESP_OK;
}

esp_err_t display_service_start(void)
{
    esp_err_t err = display_service_init();
    if (err != ESP_OK) {
        return err;
    }

    if (s_display_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "display",
                                            MIMI_DISPLAY_TASK_STACK, NULL,
                                            MIMI_DISPLAY_TASK_PRIO,
                                            &s_display_task,
                                            MIMI_DISPLAY_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create display task");
        return ESP_FAIL;
    }

    if (lock_state() == ESP_OK) {
        s_state.service_started = true;
        unlock_state();
    }
    ESP_LOGI(TAG, "display service started");
    return ESP_OK;
}

static esp_err_t request_refresh_for_dirty_region(uint32_t region_bits)
{
    if (!s_display_events) {
        return ESP_ERR_INVALID_STATE;
    }
    mark_dirty_region(region_bits);
    xEventGroupSetBits(s_display_events, DISPLAY_EVENT_REFRESH);
    return ESP_OK;
}

esp_err_t display_service_request_refresh(void)
{
    return request_refresh_for_dirty_region(DISPLAY_DIRTY_FULL);
}

static void display_boot_auto_update_task(void *arg)
{
    display_boot_update_ctx_t *ctx = (display_boot_update_ctx_t *)arg;
    char ip_address[16] = {0};
    if (ctx) {
        snprintf(ip_address, sizeof(ip_address), "%s", ctx->ip_address);
        free(ctx);
    }

    ESP_LOGI(TAG, "boot display auto-update started for ip=%s", ip_address[0] ? ip_address : "unknown");

    char time_output[128];
    esp_err_t time_err = tool_get_time_execute("{\"timezone\":\"Asia/Shanghai\"}", time_output, sizeof(time_output));
    if (time_err == ESP_OK) {
        request_refresh_for_dirty_region(DISPLAY_DIRTY_HEADER);
    } else {
        ESP_LOGW(TAG, "boot time update failed: %s", esp_err_to_name(time_err));
    }

    char city[MIMI_DISPLAY_WEATHER_CITY_LEN];
    char summary[MIMI_DISPLAY_WEATHER_SUMMARY_LEN];
    esp_err_t weather_err = fetch_weather_wttr(city, sizeof(city), summary, sizeof(summary));
    if (weather_err == ESP_OK) {
        esp_err_t set_err = display_service_set_weather(city, summary, current_epoch_or_zero());
        if (set_err == ESP_OK) {
            ESP_LOGI(TAG, "boot weather updated: %s - %s", city, summary);
        } else {
            ESP_LOGW(TAG, "boot weather update failed: %s; keeping persisted weather", esp_err_to_name(set_err));
        }
    } else {
        ESP_LOGW(TAG, "boot weather fetch failed: %s; keeping persisted weather", esp_err_to_name(weather_err));
    }

    /* Generate initial daily quote if none exists */
    {
        mimi_display_state_t snapshot;
        if (display_service_get_state(&snapshot) == ESP_OK && snapshot.quote[0] == '\0') {
            cJSON *messages = cJSON_CreateArray();
            cJSON *user_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content",
                "请生成一句关于生活哲学的中文金句，控制在16字以内。只返回金句本身，不要加引号、标点或任何说明。");
            cJSON_AddItemToArray(messages, user_msg);

            llm_response_t resp;
            memset(&resp, 0, sizeof(resp));
            esp_err_t llm_err = llm_chat_tools(
                "你是一位生活哲学家，用简洁的中文给出金句。", messages, NULL, &resp);
            cJSON_Delete(messages);

            if (llm_err == ESP_OK && resp.text && resp.text[0] != '\0') {
                char *text = resp.text;
                while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
                    text++;
                }
                size_t len = strlen(text);
                while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                                   text[len - 1] == '\r' || text[len - 1] == '\n')) {
                    len--;
                    text[len] = '\0';
                }
                if (text[0] != '\0') {
                    display_service_set_quote(text, current_epoch_or_zero());
                    ESP_LOGI(TAG, "boot quote generated: %s", text);
                }
            } else {
                ESP_LOGW(TAG, "boot quote generation skipped (llm unavailable)");
            }
            llm_response_free(&resp);
        }
    }

    vTaskDelete(NULL);
}

esp_err_t display_service_start_boot_auto_update(const char *ip_address)
{
    display_boot_update_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    if (ip_address) {
        snprintf(ctx->ip_address, sizeof(ctx->ip_address), "%s", ip_address);
    }

    BaseType_t ok = xTaskCreate(display_boot_auto_update_task, "disp_boot_update",
                                DISPLAY_BOOT_UPDATE_STACK, ctx,
                                DISPLAY_BOOT_UPDATE_PRIO, NULL);
    if (ok != pdPASS) {
        free(ctx);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t display_service_refresh_now(void)
{
    if (!display_service_is_display_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    return render_current_dashboard();
}

esp_err_t display_service_set_weather_city(const char *city)
{
    if (!city || city[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }
    mimi_copy_string_truncated_utf8(s_state.weather_city, sizeof(s_state.weather_city), city);
    s_state.weather_summary[0] = '\0';
    s_state.weather_updated_epoch = current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    request_refresh_for_dirty_region(DISPLAY_DIRTY_WEATHER);
    return err;
}

esp_err_t display_service_set_weather(const char *city, const char *summary, int64_t updated_epoch)
{
    if ((!city || city[0] == '\0') && (!summary || summary[0] == '\0')) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }
    if (city && city[0] != '\0') {
        mimi_copy_string_truncated_utf8(s_state.weather_city, sizeof(s_state.weather_city), city);
    }
    mimi_copy_string_truncated_utf8(s_state.weather_summary, sizeof(s_state.weather_summary), summary);
    s_state.weather_updated_epoch = updated_epoch > 0 ? updated_epoch : current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    request_refresh_for_dirty_region(DISPLAY_DIRTY_WEATHER);
    return err;
}

esp_err_t display_service_set_todos(const char *const *todos, size_t todo_count, int64_t updated_epoch)
{
    if (todo_count > 0 && !todos) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }

    memset(s_state.todos, 0, sizeof(s_state.todos));
    s_state.todo_count = 0;
    for (size_t i = 0; i < todo_count && s_state.todo_count < MIMI_DISPLAY_MAX_TODOS; i++) {
        if (!todos[i] || todos[i][0] == '\0') {
            continue;
        }
        mimi_copy_string_truncated_utf8(s_state.todos[s_state.todo_count], MIMI_DISPLAY_TODO_LEN, todos[i]);
        s_state.todo_count++;
    }
    s_state.todos_updated_epoch = updated_epoch > 0 ? updated_epoch : current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    request_refresh_for_dirty_region(DISPLAY_DIRTY_TODOS);
    return err;
}

esp_err_t display_service_set_quote(const char *quote, int64_t updated_epoch)
{
    if (!quote || quote[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }
    mimi_copy_string_truncated_utf8(s_state.quote, sizeof(s_state.quote), quote);
    s_state.quote_updated_epoch = updated_epoch > 0 ? updated_epoch : current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    request_refresh_for_dirty_region(DISPLAY_DIRTY_QUOTE);
    return err;
}

esp_err_t display_service_get_state(mimi_display_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = lock_state();
    if (err != ESP_OK) {
        return err;
    }
    *state = s_state;
    unlock_state();
    return ESP_OK;
}

esp_err_t display_service_get_state_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mimi_display_state_t snapshot;
    esp_err_t err = display_service_get_state(&snapshot);
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"%s\"}", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "weather_city", snapshot.weather_city);
    cJSON_AddStringToObject(root, "weather_summary", snapshot.weather_summary);
    cJSON_AddNumberToObject(root, "weather_updated_epoch", (double)snapshot.weather_updated_epoch);
    cJSON_AddNumberToObject(root, "todos_updated_epoch", (double)snapshot.todos_updated_epoch);
    cJSON_AddBoolToObject(root, "display_available", snapshot.display_available);
    cJSON_AddBoolToObject(root, "service_started", snapshot.service_started);
    cJSON_AddStringToObject(root, "quote", snapshot.quote);
    cJSON_AddNumberToObject(root, "quote_updated_epoch", (double)snapshot.quote_updated_epoch);

    cJSON *todos = cJSON_CreateArray();
    if (!todos) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < snapshot.todo_count && i < MIMI_DISPLAY_MAX_TODOS; i++) {
        cJSON_AddItemToArray(todos, cJSON_CreateString(snapshot.todos[i]));
    }
    cJSON_AddItemToObject(root, "todos", todos);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

void display_service_setup_daily_quote(void)
{
    const cron_job_t *jobs;
    int count;
    cron_list_jobs(&jobs, &count);
    for (int i = 0; i < count; i++) {
        if (strcmp(jobs[i].name, "每日金句") == 0) {
            ESP_LOGI(TAG, "daily quote cron job already exists");
            return;
        }
    }

    cron_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.name, "每日金句", sizeof(job.name) - 1);
    job.kind = CRON_KIND_EVERY;
    job.interval_s = 24 * 60 * 60;
    strncpy(job.message,
            "请在墨水屏上更新每日金句，生成一句关于生活哲学的中文金句，控制在16字以内，"
            "调用 display_set_quote 工具保存",
            sizeof(job.message) - 1);
    strncpy(job.channel, MIMI_CHAN_SYSTEM, sizeof(job.channel) - 1);
    strncpy(job.chat_id, "daily_quote", sizeof(job.chat_id) - 1);

    esp_err_t err = cron_add_job(&job);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "daily quote cron job added (id=%s)", job.id);
    } else {
        ESP_LOGW(TAG, "failed to add daily quote cron job: %s", esp_err_to_name(err));
    }
}

bool display_service_is_display_available(void)
{
    bool available = false;
    if (lock_state() == ESP_OK) {
        available = s_state.display_available;
        unlock_state();
    }
    return available;
}

esp_err_t display_service_show_image_frame(const uint8_t *framebuffer, size_t len)
{
    if (!framebuffer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != MIMI_DISPLAY_FB_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!display_service_is_display_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    return epaper_waveshare_2in9_v2_display_frame(framebuffer, len);
}

#endif /* MIMI_DISPLAY_SERVICE_WEATHER_PARSE_TEST */
