#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Single source of truth for all sensor data consumed by L1/L2 tasks.
 * All drivers write here; all consumers read from here.
 * Protected by internal mutex; hold time < 5us.
 */
typedef struct {
    /* Ultrasonic: 0=left, 1=front, 2=right */
    int distances_cm[3];
    bool distance_valid[3];
    int64_t distance_ts_us[3];

    /* IMU (MPU6050 complementary filter output) */
    float roll_deg;
    float pitch_deg;
    float yaw_deg;
    float gz_dps;
    bool imu_valid;
    int64_t imu_ts_us;

    /* GPS (NEO-6M) */
    double lat;
    double lon;
    bool gps_fix;
    int gps_sats;
    double speed_mps;
    double course_deg;
    int64_t gps_ts_us;

    /* Active navigation goal (set by nav_controller) */
    double goal_lat;
    double goal_lon;
    char goal_name[64];
    bool has_goal;
} nav_situation_t;

esp_err_t nav_situation_init(void);

void nav_situation_get(nav_situation_t *out);

void nav_situation_update_distances(int left_cm, int front_cm, int right_cm,
                                    bool left_valid, bool front_valid, bool right_valid);

void nav_situation_update_imu(float roll, float pitch, float yaw, float gz);

void nav_situation_update_gps(double lat, double lon, bool fix, int sats,
                               double speed_mps, double course_deg);

void nav_situation_set_goal(double lat, double lon, const char *name);

void nav_situation_clear_goal(void);

#ifdef __cplusplus
}
#endif
