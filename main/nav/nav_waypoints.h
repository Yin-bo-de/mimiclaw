#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAV_WAYPOINT_NAME_MAX  64
#define NAV_WAYPOINT_NOTES_MAX 128
#define NAV_WAYPOINTS_MAX      32

typedef struct {
    char name[NAV_WAYPOINT_NAME_MAX];
    double lat;
    double lon;
    uint32_t saved_at;   /* unix timestamp */
    int fix_sats;
    float hdop;
    char notes[NAV_WAYPOINT_NOTES_MAX];
} nav_waypoint_t;

/**
 * Load waypoints from SPIFFS into memory. Must be called before any other
 * nav_waypoints_* function.
 */
esp_err_t nav_waypoints_init(void);

/**
 * Save (or overwrite) a named waypoint with the given GPS coordinates.
 */
esp_err_t nav_waypoints_save(const char *name, double lat, double lon,
                              int sats, float hdop, const char *notes);

/**
 * Look up a waypoint by name. Returns ESP_ERR_NOT_FOUND if absent.
 */
esp_err_t nav_waypoints_find(const char *name, nav_waypoint_t *out);

/**
 * Delete a waypoint by name. Returns ESP_ERR_NOT_FOUND if absent.
 */
esp_err_t nav_waypoints_delete(const char *name);

/**
 * Copy up to max_count waypoints into buf. Returns actual count.
 */
int nav_waypoints_list(nav_waypoint_t *buf, int max_count);

#ifdef __cplusplus
}
#endif
