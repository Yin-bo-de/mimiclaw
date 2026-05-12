#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Raw GPS observation as received from NMEA driver. */
typedef struct {
    double  lat;        /* degrees */
    double  lon;        /* degrees */
    double  speed_mps;
    double  course_deg;
    int     sats;
    bool    fix_valid;
    int64_t ts_us;      /* esp_timer_get_time() at parse time */
} nav_gps_obs_t;

/** Kalman-filtered output pushed to nav_situation. */
typedef struct {
    double  lat;
    double  lon;
    double  speed_mps;
    double  course_deg;
    bool    fix;        /* false when degraded (prolonged predict-only) */
    int     sats;
    int     quality;    /* 0=rejected/predict-only 1=weak 2=normal 3=good */
    int64_t ts_us;
} nav_gps_filtered_t;

/** Runtime statistics for diagnostics. */
typedef struct {
    uint32_t total_obs;
    uint32_t accepted;           /* full-weight update */
    uint32_t weakened;           /* high-R update (sats 4-6) */
    uint32_t rejected;           /* predict-only (bad sats/fix) */
    uint32_t predict_only_streak;
    double   origin_lat;
    double   origin_lon;
    bool     initialized;
    bool     stationary;
} nav_gps_filter_stats_t;

/**
 * Initialize filter. Must be called AFTER nav_config_init().
 */
esp_err_t nav_gps_filter_init(void);

/**
 * Reset Kalman state and origin (e.g. for new deployment area).
 */
void nav_gps_filter_reset(void);

/**
 * Process one GPS observation. Must be called even when fix_valid=false
 * so the filter can run the predict-only branch and degrade fix properly.
 *
 * @param obs  Raw GPS observation (never NULL)
 * @param out  Filtered output (never NULL)
 */
esp_err_t nav_gps_filter_update(const nav_gps_obs_t *obs,
                                 nav_gps_filtered_t  *out);

/** Snapshot of last filtered output (thread-safe). */
void nav_gps_filter_get_last(nav_gps_filtered_t *out);

/** Copy current statistics (thread-safe). */
void nav_gps_filter_get_stats(nav_gps_filter_stats_t *out);

#ifdef __cplusplus
}
#endif
