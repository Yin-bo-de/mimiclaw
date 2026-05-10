#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ultrasonic channel indices */
typedef enum {
    ULTRASONIC_LEFT = 0,
    ULTRASONIC_FRONT = 1,
    ULTRASONIC_RIGHT = 2,
    ULTRASONIC_CHANNEL_COUNT = 3
} ultrasonic_channel_t;

/* Measurement result */
typedef struct {
    int distance_cm;           /* -1 = invalid/out of range */
    int64_t timestamp_us;      /* when this reading was taken */
    bool valid;
} ultrasonic_reading_t;

/* Initialize ultrasonic driver */
esp_err_t driver_ultrasonic_init(void);

/* Start the background measurement task (round-robin polling) */
esp_err_t driver_ultrasonic_start(void);

/* Stop the background measurement task */
esp_err_t driver_ultrasonic_stop(void);

/* Get the latest reading for a specific channel */
ultrasonic_reading_t driver_ultrasonic_get_reading(ultrasonic_channel_t ch);

/* Get all three readings at once (atomic snapshot) */
void driver_ultrasonic_get_all(ultrasonic_reading_t *left,
                               ultrasonic_reading_t *front,
                               ultrasonic_reading_t *right);

/* Trigger a single measurement on a specific channel (for testing, block ~60ms max) */
int driver_ultrasonic_measure_once(ultrasonic_channel_t ch);

#ifdef __cplusplus
}
#endif
