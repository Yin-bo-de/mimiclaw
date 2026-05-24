#pragma once

#include "esp_err.h"
#include "mimi_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIMI_DISPLAY_WEATHER_CITY_LEN      64
#define MIMI_DISPLAY_WEATHER_SUMMARY_LEN   128
#define MIMI_DISPLAY_MAX_TODOS             5
#define MIMI_DISPLAY_TODO_LEN              64
#define MIMI_DISPLAY_QUOTE_LEN             128

typedef struct {
    char weather_city[MIMI_DISPLAY_WEATHER_CITY_LEN];
    char weather_summary[MIMI_DISPLAY_WEATHER_SUMMARY_LEN];
    int64_t weather_updated_epoch;
    char todos[MIMI_DISPLAY_MAX_TODOS][MIMI_DISPLAY_TODO_LEN];
    size_t todo_count;
    int64_t todos_updated_epoch;
    char quote[MIMI_DISPLAY_QUOTE_LEN];
    int64_t quote_updated_epoch;
    bool display_available;
    bool service_started;
} mimi_display_state_t;

/**
 * Initialize dashboard state and persistence.
 *
 * Hardware display initialization is performed by the background display task so
 * app startup is not blocked by an absent or busy e-paper panel.
 */
esp_err_t display_service_init(void);

/**
 * Start the low-priority display task. Safe to call more than once.
 */
esp_err_t display_service_start(void);

/**
 * Request an asynchronous dashboard refresh.
 */
esp_err_t display_service_request_refresh(void);

esp_err_t display_service_start_boot_auto_update(const char *ip_address);

/**
 * Render the current dashboard immediately when display hardware is available.
 */
esp_err_t display_service_refresh_now(void);

/**
 * Set only the weather city and persist display state.
 */
esp_err_t display_service_set_weather_city(const char *city);

/**
 * Set weather city/summary and persist display state.
 */
esp_err_t display_service_set_weather(const char *city, const char *summary, int64_t updated_epoch);

/**
 * Replace the displayed todo list and persist display state.
 */
esp_err_t display_service_set_todos(const char *const *todos, size_t todo_count, int64_t updated_epoch);

/**
 * Set the daily quote and persist display state.
 */
esp_err_t display_service_set_quote(const char *quote, int64_t updated_epoch);

/**
 * Ensure a daily cron job exists that triggers quote refresh.
 * Safe to call multiple times (no-op if job already exists).
 */
void display_service_setup_daily_quote(void);

/**
 * Copy the current state into caller-owned storage.
 */
esp_err_t display_service_get_state(mimi_display_state_t *state);

/**
 * Serialize the current state as compact JSON into caller-owned storage.
 */
esp_err_t display_service_get_state_json(char *output, size_t output_size);

/**
 * Return whether the display driver is currently available.
 */
bool display_service_is_display_available(void);

#ifdef __cplusplus
}
#endif
