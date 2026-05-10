#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Haversine great-circle distance between two GPS coordinates.
 * Returns distance in meters.
 */
double nav_planner_distance_m(double lat1, double lon1, double lat2, double lon2);

/**
 * Initial bearing from point 1 to point 2 (degrees, 0=North, clockwise).
 */
double nav_planner_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/**
 * Signed heading error: target_bearing - current_yaw, normalized to [-180, +180].
 * Positive = need to turn right.
 */
double nav_planner_heading_error_deg(double current_yaw_deg, double target_bearing_deg);

#ifdef __cplusplus
}
#endif
