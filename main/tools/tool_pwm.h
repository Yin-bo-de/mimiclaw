#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize PWM/Servo subsystem — reserve LEDC channels.
 */
esp_err_t tool_pwm_init(void);

/**
 * Set servo angle on a GPIO pin.
 * Input JSON: {"gpio": <int>, "angle": <int 0-180>}
 */
esp_err_t tool_servo_set_execute(const char *input_json, char *output, size_t output_size);

/**
 * Stop PWM output and release the servo on a GPIO pin.
 * Input JSON: {"gpio": <int>}
 */
esp_err_t tool_servo_release_execute(const char *input_json, char *output, size_t output_size);
