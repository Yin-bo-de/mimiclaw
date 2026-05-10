#include "nav/nav_memory.h"
#include "nav/nav_planner.h"

#include "esp_timer.h"

#include <string.h>

static avoid_record_t s_buf[NAV_MEMORY_SIZE];
static int s_head  = 0;  /* next write slot */
static int s_count = 0;

void nav_memory_init(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_head  = 0;
    s_count = 0;
}

void nav_memory_record_attempt(double lat, double lon, int side, int d_front_cm)
{
    avoid_record_t *r = &s_buf[s_head];
    r->lat        = lat;
    r->lon        = lon;
    r->side       = side;
    r->d_front_cm = d_front_cm;
    r->succeeded  = false;
    r->duration_ms = 0;
    r->ts_us      = esp_timer_get_time();
    r->valid      = true;

    s_head = (s_head + 1) % NAV_MEMORY_SIZE;
    if (s_count < NAV_MEMORY_SIZE) s_count++;
}

void nav_memory_mark_last_result(bool succeeded, uint32_t duration_ms)
{
    if (s_count == 0) return;
    int last = (s_head - 1 + NAV_MEMORY_SIZE) % NAV_MEMORY_SIZE;
    s_buf[last].succeeded   = succeeded;
    s_buf[last].duration_ms = duration_ms;
}

float nav_memory_penalty_for_side(int side, double lat, double lon)
{
    int total = 0, failed = 0;
    for (int i = 0; i < s_count; i++) {
        /* iterate oldest→newest */
        int idx = (s_head - s_count + i + NAV_MEMORY_SIZE * 2) % NAV_MEMORY_SIZE;
        const avoid_record_t *r = &s_buf[idx];
        if (!r->valid || r->side != side) continue;
        if (nav_planner_distance_m(lat, lon, r->lat, r->lon) > NAV_MEMORY_PROXIMITY_M) continue;
        total++;
        if (!r->succeeded) failed++;
    }
    if (total == 0) return 0.0f;
    return (float)failed / (float)total;
}

const avoid_record_t *nav_memory_get(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    int pos = (s_head - s_count + idx + NAV_MEMORY_SIZE * 2) % NAV_MEMORY_SIZE;
    return &s_buf[pos];
}

int nav_memory_count(void) { return s_count; }
