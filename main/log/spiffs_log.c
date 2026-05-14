#include "spiffs_log.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"

/* /spiffs(8) + '/'(1) + d_name(SPIFFS max ~255) + NUL */
#define PATH_BUF 280

static FILE              *s_file      = NULL;
static SemaphoreHandle_t  s_mutex     = NULL;
static vprintf_like_t     s_orig_vp   = NULL;
static size_t             s_file_size = 0;
static char               s_filename[PATH_BUF];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t bump_boot_counter(void)
{
    nvs_handle_t h;
    uint32_t cnt = 0;
    if (nvs_open(MIMI_NVS_LOG, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, MIMI_NVS_KEY_BOOT_CNT, &cnt);
        nvs_set_u32(h, MIMI_NVS_KEY_BOOT_CNT, cnt + 1);
        nvs_commit(h);
        nvs_close(h);
    }
    return cnt;
}

static bool is_log_entry(const char *d_name)
{
    return strncmp(d_name, "logs/run_", 9) == 0;
}

static uint32_t log_entry_num(const char *d_name)
{
    if (!is_log_entry(d_name)) return 0;
    uint32_t n = 0;
    sscanf(d_name + 9, "%" SCNu32, &n);
    return n;
}

static void spiffs_usage(size_t *total, size_t *used)
{
    *total = 0;
    *used  = 0;
    esp_spiffs_info(NULL, total, used);
}

static bool near_full(void)
{
    size_t total, used;
    spiffs_usage(&total, &used);
    if (!total) return false;
    return (used * 100 / total) >= MIMI_LOG_SPIFFS_FULL_PCT;
}

/* Delete the oldest log file (smallest run number), never the current one */
static void evict_oldest(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) return;

    uint32_t   oldest_num  = UINT32_MAX;
    char       oldest_path[PATH_BUF] = {0};
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        uint32_t n = log_entry_num(ent->d_name);
        if (!n) continue;
        if (n < oldest_num) {
            oldest_num = n;
            snprintf(oldest_path, PATH_BUF,
                     MIMI_SPIFFS_BASE "/%s", ent->d_name);
        }
    }
    closedir(dir);

    /* Avoid deleting the file we are currently writing */
    if (oldest_path[0] && strcmp(oldest_path, s_filename) != 0) {
        unlink(oldest_path);
        printf("[spiffs_log] Evicted oldest log: %s\n", oldest_path);
    }
}

static void open_log_file(uint32_t boot_cnt)
{
    snprintf(s_filename, sizeof(s_filename),
             MIMI_SPIFFS_BASE "/logs/run_%04u.log", (unsigned)boot_cnt);

    if (near_full()) evict_oldest();

    s_file      = fopen(s_filename, "w");
    s_file_size = 0;

    if (!s_file) {
        printf("[spiffs_log] Cannot open log file: %s\n", s_filename);
    }
}

/* ------------------------------------------------------------------ */
/* vprintf hook — called for every ESP_LOGx invocation                */
/* ------------------------------------------------------------------ */

static int log_hook(const char *fmt, va_list args)
{
    /* Keep a copy so we can use args twice */
    va_list args_copy;
    va_copy(args_copy, args);

    /* Forward to original serial output first */
    int ret = s_orig_vp ? s_orig_vp(fmt, args) : vprintf(fmt, args);

    /* Write to SPIFFS file (non-blocking: drop if mutex busy > 5 ms) */
    if (s_file && s_mutex) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            int n = vfprintf(s_file, fmt, args_copy);
            if (n > 0) {
                s_file_size += (size_t)n;
                /* Flush every line so a crash leaves the log intact */
                fflush(s_file);

                if (s_file_size >= MIMI_LOG_MAX_FILE_BYTES) {
                    fclose(s_file);
                    s_file = NULL;
                    printf("[spiffs_log] Max size reached: %s\n", s_filename);
                }
            }
            xSemaphoreGive(s_mutex);
        }
    }

    va_end(args_copy);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t spiffs_log_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    uint32_t boot_cnt = bump_boot_counter();
    open_log_file(boot_cnt);

    /* Install hook after file is ready — any failure above is non-fatal */
    s_orig_vp = esp_log_set_vprintf(log_hook);

    if (s_file) {
        printf("[spiffs_log] Logging to SPIFFS: %s\n", s_filename);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* CLI helpers                                                         */
/* ------------------------------------------------------------------ */

void spiffs_log_list(void)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        printf("Cannot open SPIFFS.\n");
        return;
    }

    printf("%-38s  %8s\n", "File", "Size");
    printf("%-38s  %8s\n", "----", "----");

    int    count           = 0;
    size_t total_log_bytes = 0;
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (!is_log_entry(ent->d_name)) continue;

        char path[PATH_BUF];
        snprintf(path, PATH_BUF, MIMI_SPIFFS_BASE "/%s", ent->d_name);

        struct stat st;
        size_t sz = 0;
        if (stat(path, &st) == 0) sz = (size_t)st.st_size;

        const char *active = strcmp(path, s_filename) == 0 ? " [active]" : "";
        printf("  %-36s  %6zu B%s\n", ent->d_name, sz, active);
        total_log_bytes += sz;
        count++;
    }
    closedir(dir);

    if (count == 0) {
        printf("  (no log files)\n");
    } else {
        printf("  Total: %d file(s), %zu bytes\n", count, total_log_bytes);
    }

    size_t sp_total, sp_used;
    spiffs_usage(&sp_total, &sp_used);
    printf("SPIFFS: %zu / %zu bytes used (%zu%%)\n",
           sp_used, sp_total,
           sp_total ? sp_used * 100 / sp_total : 0);
}

esp_err_t spiffs_log_read(const char *filename)
{
    /* Accept "run_0001.log" or "logs/run_0001.log" */
    char path[PATH_BUF];
    if (strncmp(filename, "logs/", 5) == 0) {
        snprintf(path, PATH_BUF, MIMI_SPIFFS_BASE "/%s", filename);
    } else {
        snprintf(path, PATH_BUF, MIMI_SPIFFS_BASE "/logs/%s", filename);
    }

    if (strstr(path, "..")) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("File not found: %s\n", path);
        return ESP_ERR_NOT_FOUND;
    }

    printf("=== %s ===\n", path);
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        fputs(buf, stdout);
    }
    fclose(f);
    printf("\n=== END ===\n");
    return ESP_OK;
}

esp_err_t spiffs_log_delete(const char *filename)
{
    char path[PATH_BUF];
    if (strncmp(filename, "logs/", 5) == 0) {
        snprintf(path, PATH_BUF, MIMI_SPIFFS_BASE "/%s", filename);
    } else {
        snprintf(path, PATH_BUF, MIMI_SPIFFS_BASE "/logs/%s", filename);
    }

    if (strstr(path, "..")) return ESP_ERR_INVALID_ARG;

    /* Close the active file if the user is deleting it */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (s_file && strcmp(path, s_filename) == 0) {
            fclose(s_file);
            s_file = NULL;
        }
        xSemaphoreGive(s_mutex);
    }

    if (unlink(path) != 0) {
        printf("Delete failed: %s\n", path);
        return ESP_FAIL;
    }
    printf("Deleted: %s\n", path);
    return ESP_OK;
}

void spiffs_log_clear(void)
{
    /* Pause logging */
    if (s_mutex) xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200));

    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }

    if (s_mutex) xSemaphoreGive(s_mutex);

    /* Collect paths before deleting (avoid modifying dir while iterating) */
    char paths[MIMI_LOG_MAX_FILES][PATH_BUF];
    int  path_count = 0;

    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && path_count < MIMI_LOG_MAX_FILES) {
            if (!is_log_entry(ent->d_name)) continue;
            snprintf(paths[path_count], PATH_BUF,
                     MIMI_SPIFFS_BASE "/%s", ent->d_name);
            path_count++;
        }
        closedir(dir);
    }

    int deleted = 0;
    for (int i = 0; i < path_count; i++) {
        if (unlink(paths[i]) == 0) {
            deleted++;
            printf("Deleted: %s\n", paths[i]);
        }
    }
    printf("Cleared %d log file(s).\n", deleted);

    /* Reopen for continued logging in this session */
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_file      = fopen(s_filename, "w");
        s_file_size = 0;
        xSemaphoreGive(s_mutex);
    }
    if (s_file) {
        printf("[spiffs_log] Resumed logging: %s\n", s_filename);
    }
}

void spiffs_log_status(void)
{
    size_t total, used;
    spiffs_usage(&total, &used);
    size_t free_bytes = total > used ? total - used : 0;
    printf("SPIFFS total : %zu bytes (%.1f MB)\n", total, total / 1048576.0f);
    printf("SPIFFS used  : %zu bytes\n", used);
    printf("SPIFFS free  : %zu bytes\n", free_bytes);
    printf("Usage        : %zu%%\n", total ? used * 100 / total : 0);
    printf("Current log  : %s\n", s_filename[0] ? s_filename : "(none)");
    printf("Written      : %zu bytes\n", s_file_size);
    printf("File open    : %s\n", s_file ? "yes" : "no (full or error)");
}
