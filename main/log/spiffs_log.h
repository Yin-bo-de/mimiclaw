#pragma once
#include "esp_err.h"

/**
 * @brief Install vprintf hook and open /spiffs/logs/run_XXXX.log.
 *        Must be called after esp_vfs_spiffs_register().
 */
esp_err_t spiffs_log_init(void);

/* CLI helpers — print results to stdout */
void      spiffs_log_list(void);
esp_err_t spiffs_log_read(const char *filename);
esp_err_t spiffs_log_delete(const char *filename);
void      spiffs_log_clear(void);
void      spiffs_log_status(void);
