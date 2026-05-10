#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GPS reading */
typedef struct {
    double latitude;         /* Latitude in degrees, positive = North */
    double longitude;        /* Longitude in degrees, positive = East */
    double altitude_m;       /* Altitude in meters */
    double speed_mps;        /* Speed in meters per second */
    double course_deg;       /* Course over ground in degrees (0-360) */
    int satellites;          /* Number of satellites in view */
    bool fix_valid;          /* True if GPS fix is valid */
    int64_t timestamp_us;    /* Time of reading */
} gps_reading_t;

/* Initialize GPS driver */
esp_err_t driver_gps_init(void);

/* Start the background measurement task */
esp_err_t driver_gps_start(void);

/* Stop the background measurement task */
esp_err_t driver_gps_stop(void);

/* Get the latest reading */
gps_reading_t driver_gps_get_reading(void);

/* Enable/disable NMEA raw sentence debug output */
void driver_gps_set_debug(bool enable);

#ifdef __cplusplus
}
#endif
