#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * HTTP GET handler for /ui — returns the embedded WebUI HTML page.
 */
esp_err_t webui_handler(httpd_req_t *req);
