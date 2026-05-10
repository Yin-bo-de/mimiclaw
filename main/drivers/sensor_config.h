#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ultrasonic sensor channel config */
typedef struct {
    int trig_gpio;
    int echo_gpio;
} ultrasonic_channel_config_t;

typedef struct {
    ultrasonic_channel_config_t left;
    ultrasonic_channel_config_t front;
    ultrasonic_channel_config_t right;
    int max_range_cm;
    int round_robin_gap_ms;
    bool loaded;
} ultrasonic_config_t;

/* Get ultrasonic config (Phase 1 only, others added later) */
const ultrasonic_config_t *sensor_config_get_ultrasonic(void);

/* Load sensors.json config from SPIFFS */
esp_err_t sensor_config_load(void);

/* Initialize sensor config (loads from SPIFFS on first call) */
esp_err_t sensor_config_init(void);

#ifdef __cplusplus
}
#endif
