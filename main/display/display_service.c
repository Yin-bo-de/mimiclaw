#include "display/display_service.h"

#include "display/display_render.h"
#include "display/epaper_waveshare_2in9_v2.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "display_service";

#define DISPLAY_EVENT_REFRESH BIT0
#define DISPLAY_STATE_TMP_FILE MIMI_DISPLAY_STATE_FILE ".tmp"

static mimi_display_state_t s_state;
static SemaphoreHandle_t s_state_mutex;
static EventGroupHandle_t s_display_events;
static TaskHandle_t s_display_task;
static bool s_initialized;
static bool s_driver_attempted;
static uint8_t s_framebuffer[MIMI_DISPLAY_FB_BYTES];

static void copy_string_truncated(char *dest, size_t dest_size, const char *src)
{
    if (!dest || dest_size == 0) {
        return;
    }
    if (!src) {
        dest[0] = '\0';
        return;
    }
    snprintf(dest, dest_size, "%s", src);
}

static int64_t current_epoch_or_zero(void)
{
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
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

    copy_string_truncated(s_state.weather_city, sizeof(s_state.weather_city), weather_city);
    copy_string_truncated(s_state.weather_summary, sizeof(s_state.weather_summary), weather_summary);
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
            copy_string_truncated(s_state.todos[s_state.todo_count], MIMI_DISPLAY_TODO_LEN, todo);
            s_state.todo_count++;
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void format_local_datetime(char *date, size_t date_size, char *time_text, size_t time_size)
{
    time_t now = time(NULL);
    struct tm local;

    if (now <= 0 || !localtime_r(&now, &local)) {
        if (date_size > 0) {
            date[0] = '\0';
        }
        snprintf(time_text, time_size, "Time syncing...");
        return;
    }

    strftime(date, date_size, "%Y-%m-%d", &local);
    strftime(time_text, time_size, "%H:%M", &local);
}

static esp_err_t render_current_dashboard(void)
{
#if MIMI_DISPLAY_ENABLED
    if (!s_state.display_available) {
        return ESP_ERR_INVALID_STATE;
    }

    mimi_display_state_t snapshot;
    esp_err_t err = display_service_get_state(&snapshot);
    if (err != ESP_OK) {
        return err;
    }

    char date[16];
    char time_text[16];
    format_local_datetime(date, sizeof(date), time_text, sizeof(time_text));

    display_dashboard_data_t data = {
        .date = date,
        .time = time_text,
        .weather_city = snapshot.weather_city,
        .weather_summary = snapshot.weather_summary,
        .todo_count = snapshot.todo_count,
    };
    for (size_t i = 0; i < snapshot.todo_count && i < MIMI_DISPLAY_MAX_TODOS; i++) {
        data.todos[i] = snapshot.todos[i];
    }

    display_render_dashboard(s_framebuffer, sizeof(s_framebuffer), &data);
    return epaper_waveshare_2in9_v2_display_frame(s_framebuffer, sizeof(s_framebuffer));
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
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

        esp_err_t err = render_current_dashboard();
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

esp_err_t display_service_request_refresh(void)
{
    if (!s_display_events) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupSetBits(s_display_events, DISPLAY_EVENT_REFRESH);
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
    copy_string_truncated(s_state.weather_city, sizeof(s_state.weather_city), city);
    s_state.weather_summary[0] = '\0';
    s_state.weather_updated_epoch = current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    display_service_request_refresh();
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
        copy_string_truncated(s_state.weather_city, sizeof(s_state.weather_city), city);
    }
    copy_string_truncated(s_state.weather_summary, sizeof(s_state.weather_summary), summary);
    s_state.weather_updated_epoch = updated_epoch > 0 ? updated_epoch : current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    display_service_request_refresh();
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
        copy_string_truncated(s_state.todos[s_state.todo_count], MIMI_DISPLAY_TODO_LEN, todos[i]);
        s_state.todo_count++;
    }
    s_state.todos_updated_epoch = updated_epoch > 0 ? updated_epoch : current_epoch_or_zero();
    err = save_state_locked();
    unlock_state();

    display_service_request_refresh();
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

bool display_service_is_display_available(void)
{
    bool available = false;
    if (lock_state() == ESP_OK) {
        available = s_state.display_available;
        unlock_state();
    }
    return available;
}
