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

/* IMU (MPU6050) config */
typedef struct {
    int i2c_port;
    int sda_gpio;
    int scl_gpio;
    uint8_t address;
    int freq_hz;
    int sample_hz;
    float gyro_bias_dps[3]; /* [x, y, z] */
    bool loaded;
} imu_config_t;

/* GPS (NEO-6M) config */
typedef struct {
    int uart_port;
    int rx_gpio;
    int tx_gpio;
    int baudrate;
    bool loaded;
} gps_config_t;

/* Get ultrasonic config */
const ultrasonic_config_t *sensor_config_get_ultrasonic(void);

/* Get IMU config */
const imu_config_t *sensor_config_get_imu(void);

/* Get GPS config */
const gps_config_t *sensor_config_get_gps(void);

/* Load sensors.json config from SPIFFS */
esp_err_t sensor_config_load(void);

/* Save IMU gyro bias to sensors.json (persists across reboots) */
esp_err_t sensor_config_save_imu_bias(const float bias_dps[3]);

/* Initialize sensor config (loads from SPIFFS on first call) */
esp_err_t sensor_config_init(void);

#ifdef __cplusplus
}
#endif
