#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAV_MEMORY_SIZE        8
#define NAV_MEMORY_PROXIMITY_M 10.0  /* records within this range count toward penalty */

typedef struct {
    double lat, lon;
    int    side;          /* 0=left, 1=right */
    bool   succeeded;     /* set by nav_memory_mark_last_result */
    int    d_front_cm;
    int64_t ts_us;
    uint32_t duration_ms;
    bool   valid;
} avoid_record_t;

void nav_memory_init(void);

/** Record a new avoidance attempt when entering AVOID state. */
void nav_memory_record_attempt(double lat, double lon, int side, int d_front_cm);

/** Update the last record when leaving AVOID state. */
void nav_memory_mark_last_result(bool succeeded, uint32_t duration_ms);

/**
 * Return penalty score [0.0, 1.0] for choosing `side` at `(lat, lon)`.
 * Considers only records within NAV_MEMORY_PROXIMITY_M.
 * penalty = failed_count / total_count in the area.
 */
float nav_memory_penalty_for_side(int side, double lat, double lon);

/** Index 0 = oldest record; returns NULL if idx >= count. */
const avoid_record_t *nav_memory_get(int idx);
int nav_memory_count(void);

#ifdef __cplusplus
}
#endif
