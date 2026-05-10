#include "nav/nav_planner.h"

#include <math.h>

#define DEG_TO_RAD (M_PI / 180.0)
#define RAD_TO_DEG (180.0 / M_PI)
#define EARTH_RADIUS_M 6371000.0

double nav_planner_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG_TO_RAD;
    double dlon = (lon2 - lon1) * DEG_TO_RAD;
    double a = sin(dlat / 2.0) * sin(dlat / 2.0)
             + cos(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD)
             * sin(dlon / 2.0) * sin(dlon / 2.0);
    return EARTH_RADIUS_M * 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
}

double nav_planner_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double dlon = (lon2 - lon1) * DEG_TO_RAD;
    double y = sin(dlon) * cos(lat2 * DEG_TO_RAD);
    double x = cos(lat1 * DEG_TO_RAD) * sin(lat2 * DEG_TO_RAD)
             - sin(lat1 * DEG_TO_RAD) * cos(lat2 * DEG_TO_RAD) * cos(dlon);
    double bearing = atan2(y, x) * RAD_TO_DEG;
    return fmod(bearing + 360.0, 360.0);
}

double nav_planner_heading_error_deg(double current_yaw_deg, double target_bearing_deg)
{
    double error = target_bearing_deg - current_yaw_deg;
    while (error >  180.0) error -= 360.0;
    while (error < -180.0) error += 360.0;
    return error;
}
