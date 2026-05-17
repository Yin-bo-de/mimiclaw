#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IMU reading */
typedef struct {
    float accel_mps2[3]; /* x, y, z */
    float gyro_dps[3];   /* x, y, z */
    float roll_deg;      /* roll angle */
    float pitch_deg;     /* pitch angle */
    float yaw_deg;       /* yaw angle */
    int64_t timestamp_us;
    bool valid;
} imu_reading_t;

/* Initialize IMU driver */
esp_err_t driver_imu_init(void);

/* Start the background measurement task */
esp_err_t driver_imu_start(void);

/* Stop the background measurement task */
esp_err_t driver_imu_stop(void);

/* Get the latest reading */
imu_reading_t driver_imu_get_reading(void);

/* Calibrate gyroscope bias (blocks for ~5 seconds) */
esp_err_t driver_imu_calibrate_gyro(void);

/**
 * Force yaw to a known compass bearing (deg, 0=North CW+).
 * Used by nav bootstrap to align gyro-integrated yaw with GPS COG.
 * Thread-safe; takes effect on the next IMU update cycle (~10 ms).
 */
void driver_imu_set_yaw(float deg);

/* Magnetometer (HMC5883L) status */
typedef struct {
    bool online;
    float raw_x;
    float raw_y;
    float raw_z;
    float heading_deg;   /* 0=North CW+, after declination & offset */
    float declination_deg;
    float offset_x;
    float offset_y;
} mag_status_t;

mag_status_t driver_imu_get_mag_status(void);

/**
 * Calibrate magnetometer hard-iron offset.
 * Keep board level and slowly rotate 360° during the ~15s window.
 */
esp_err_t driver_imu_calibrate_mag(void);

#ifdef __cplusplus
}
#endif
