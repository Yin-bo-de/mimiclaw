#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "gateway/webui_display.h"
#include "display/display_service.h"
#include "tools/gpio_policy.h"
#include "tools/tool_files.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "mbedtls/base64.h"

static const char *TAG = "ws";

static httpd_handle_t s_server = NULL;

/* Simple client tracking */
typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[MIMI_WS_MAX_CLIENTS];

static ws_client_t *find_client_by_fd(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *find_client_by_chat_id(const char *chat_id)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0) {
            return &s_clients[i];
        }
    }
    return NULL;
}

static ws_client_t *add_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            s_clients[i].active = true;
            ESP_LOGI(TAG, "Client connected: %s (fd=%d)", s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    ESP_LOGW(TAG, "Max clients reached, rejecting fd=%d", fd);
    return NULL;
}

static void remove_client(int fd)
{
    for (int i = 0; i < MIMI_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            ESP_LOGI(TAG, "Client disconnected: %s", s_clients[i].chat_id);
            s_clients[i].active = false;
            return;
        }
    }
}

static esp_err_t ws_send_json_to_fd(int fd, const char *json_str)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;
    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };
    return httpd_ws_send_frame_async(s_server, fd, &ws_pkt);
}

static void send_display_ack(int fd, const char *action, const char *status, const char *message)
{
    cJSON *ack = cJSON_CreateObject();
    if (!ack) return;
    cJSON_AddStringToObject(ack, "type", "display_ack");
    cJSON_AddStringToObject(ack, "action", action);
    cJSON_AddStringToObject(ack, "status", status);
    if (message && message[0]) {
        cJSON_AddStringToObject(ack, "message", message);
    }
    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (json) {
        ws_send_json_to_fd(fd, json);
        free(json);
    }
}

/* ── Path validation (copied from tool_files.c) ─────────────────────────── */

static bool validate_path(const char *path)
{
    if (!path) return false;
    size_t base_len = strlen(MIMI_SPIFFS_BASE);
    if (strncmp(path, MIMI_SPIFFS_BASE, base_len) != 0) return false;
    if (base_len > 0 && MIMI_SPIFFS_BASE[base_len - 1] != '/') {
        if (path[base_len] != '/') return false;
    }
    if (strstr(path, "..") != NULL) return false;
    return true;
}

/* ── File message handler ──────────────────────────────────────────────── */

static void send_file_ack(int fd, const char *action, const char *status,
                          cJSON *files_arr, const char *content, const char *message)
{
    cJSON *ack = cJSON_CreateObject();
    if (!ack) return;
    cJSON_AddStringToObject(ack, "type", "file_ack");
    cJSON_AddStringToObject(ack, "action", action);
    cJSON_AddStringToObject(ack, "status", status);
    if (files_arr) {
        cJSON_AddItemToObject(ack, "files", files_arr);
    }
    if (content) {
        cJSON_AddStringToObject(ack, "content", content);
    }
    if (message && message[0]) {
        cJSON_AddStringToObject(ack, "message", message);
    }
    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (json) {
        ws_send_json_to_fd(fd, json);
        free(json);
    }
}

static void handle_file_message(int fd, cJSON *root)
{
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        send_file_ack(fd, "unknown", "error", NULL, NULL, "missing action");
        return;
    }
    const char *act = action->valuestring;

    if (strcmp(act, "list") == 0) {
        DIR *dir = opendir(MIMI_SPIFFS_BASE);
        if (!dir) {
            send_file_ack(fd, act, "error", NULL, NULL, "cannot open directory");
            return;
        }
        cJSON *files = cJSON_CreateArray();
        if (!files) {
            closedir(dir);
            send_file_ack(fd, act, "error", NULL, NULL, "out of memory");
            return;
        }
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, ent->d_name);
            cJSON *path_str = cJSON_CreateString(full_path);
            if (path_str) {
                cJSON_AddItemToArray(files, path_str);
            }
        }
        closedir(dir);
        send_file_ack(fd, act, "ok", files, NULL, NULL);
        return;
    }

    cJSON *path_item = cJSON_GetObjectItem(root, "path");
    const char *path = cJSON_GetStringValue(path_item);
    if (!validate_path(path)) {
        send_file_ack(fd, act, "error", NULL, NULL, "invalid path");
        return;
    }

    if (strcmp(act, "read") == 0) {
        FILE *f = fopen(path, "r");
        if (!f) {
            send_file_ack(fd, act, "error", NULL, NULL, "file not found");
            return;
        }
        char *buf = malloc((32 * 1024) + 1);
        if (!buf) {
            fclose(f);
            send_file_ack(fd, act, "error", NULL, NULL, "out of memory");
            return;
        }
        size_t n = fread(buf, 1, 32 * 1024, f);
        buf[n] = '\0';
        fclose(f);
        send_file_ack(fd, act, "ok", NULL, buf, NULL);
        free(buf);
        return;
    }

    if (strcmp(act, "write") == 0) {
        cJSON *content_item = cJSON_GetObjectItem(root, "content");
        const char *content = cJSON_GetStringValue(content_item);
        if (!content) {
            send_file_ack(fd, act, "error", NULL, NULL, "missing content");
            return;
        }
        cJSON *input = cJSON_CreateObject();
        cJSON_AddStringToObject(input, "path", path);
        cJSON_AddStringToObject(input, "content", content);
        char *input_json = cJSON_PrintUnformatted(input);
        cJSON_Delete(input);
        if (!input_json) {
            send_file_ack(fd, act, "error", NULL, NULL, "out of memory");
            return;
        }
        char tool_output[256];
        esp_err_t err = tool_write_file_execute(input_json, tool_output, sizeof(tool_output));
        free(input_json);
        if (err == ESP_OK) {
            send_file_ack(fd, act, "ok", NULL, NULL, NULL);
        } else {
            send_file_ack(fd, act, "error", NULL, NULL, tool_output);
        }
        return;
    }

    if (strcmp(act, "delete") == 0) {
        if (remove(path) == 0) {
            send_file_ack(fd, act, "ok", NULL, NULL, NULL);
        } else {
            send_file_ack(fd, act, "error", NULL, NULL, "remove failed");
        }
        return;
    }

    send_file_ack(fd, act, "error", NULL, NULL, "unknown action");
}

/* ── GPIO message handler ──────────────────────────────────────────────── */

static void send_gpio_ack(int fd, const char *action, const char *status,
                          int pin, int state, cJSON *pins_arr, const char *message)
{
    cJSON *ack = cJSON_CreateObject();
    if (!ack) return;
    cJSON_AddStringToObject(ack, "type", "gpio_ack");
    cJSON_AddStringToObject(ack, "action", action);
    cJSON_AddStringToObject(ack, "status", status);
    if (pin >= 0) {
        cJSON_AddNumberToObject(ack, "pin", pin);
    }
    if (state >= 0) {
        cJSON_AddNumberToObject(ack, "state", state);
    }
    if (pins_arr) {
        cJSON_AddItemToObject(ack, "pins", pins_arr);
    }
    if (message && message[0]) {
        cJSON_AddStringToObject(ack, "message", message);
    }
    char *json = cJSON_PrintUnformatted(ack);
    cJSON_Delete(ack);
    if (json) {
        ws_send_json_to_fd(fd, json);
        free(json);
    }
}

static void handle_gpio_message(int fd, cJSON *root)
{
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        send_gpio_ack(fd, "unknown", "error", -1, -1, NULL, "missing action");
        return;
    }
    const char *act = action->valuestring;

    if (strcmp(act, "read_all") == 0) {
        cJSON *pins = cJSON_CreateArray();
        if (!pins) {
            send_gpio_ack(fd, act, "error", -1, -1, NULL, "out of memory");
            return;
        }
        char csv_buf[256];
        strncpy(csv_buf, MIMI_GPIO_ALLOWED_CSV, sizeof(csv_buf) - 1);
        csv_buf[sizeof(csv_buf) - 1] = '\0';
        char *saveptr = NULL;
        char *token = strtok_r(csv_buf, ",", &saveptr);
        while (token) {
            int pin = (int)strtol(token, NULL, 10);
            if (gpio_policy_pin_is_allowed(pin)) {
                gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
                int level = gpio_get_level((gpio_num_t)pin);
                cJSON *item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "pin", pin);
                cJSON_AddNumberToObject(item, "state", level);
                cJSON_AddItemToArray(pins, item);
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
        send_gpio_ack(fd, act, "ok", -1, -1, pins, NULL);
        return;
    }

    cJSON *pin_item = cJSON_GetObjectItem(root, "pin");
    if (!pin_item || !cJSON_IsNumber(pin_item)) {
        send_gpio_ack(fd, act, "error", -1, -1, NULL, "missing pin");
        return;
    }
    int pin = (int)pin_item->valuedouble;

    if (!gpio_policy_pin_is_allowed(pin)) {
        char hint[128];
        if (gpio_policy_pin_forbidden_hint(pin, hint, sizeof(hint))) {
            send_gpio_ack(fd, act, "error", pin, -1, NULL, hint);
        } else {
            send_gpio_ack(fd, act, "error", pin, -1, NULL, "pin not allowed");
        }
        return;
    }

    if (strcmp(act, "read") == 0) {
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        int level = gpio_get_level((gpio_num_t)pin);
        send_gpio_ack(fd, act, "ok", pin, level, NULL, NULL);
        return;
    }

    if (strcmp(act, "write") == 0) {
        cJSON *state_item = cJSON_GetObjectItem(root, "state");
        if (!state_item || !cJSON_IsNumber(state_item)) {
            send_gpio_ack(fd, act, "error", pin, -1, NULL, "missing state");
            return;
        }
        int level = (int)state_item->valuedouble;
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)pin, level);
        send_gpio_ack(fd, act, "ok", pin, level, NULL, NULL);
        return;
    }

    send_gpio_ack(fd, act, "error", pin, -1, NULL, "unknown action");
}

/* ── Display message handler ───────────────────────────────────────────── */

static void handle_display_message(int fd, cJSON *root)
{
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!action || !cJSON_IsString(action)) {
        send_display_ack(fd, "unknown", "error", "missing action");
        return;
    }
    const char *act = action->valuestring;
    esp_err_t err = ESP_OK;

    if (strcmp(act, "get_state") == 0) {
        char state_json[1024];
        err = display_service_get_state_json(state_json, sizeof(state_json));
        if (err == ESP_OK) {
            char resp[1280];
            snprintf(resp, sizeof(resp), "{\"type\":\"display_state\",\"state\":%s}", state_json);
            ws_send_json_to_fd(fd, resp);
        } else {
            send_display_ack(fd, act, "error", esp_err_to_name(err));
        }
        return;
    } else if (strcmp(act, "set_weather") == 0) {
        cJSON *city = cJSON_GetObjectItem(root, "city");
        cJSON *summary = cJSON_GetObjectItem(root, "summary");
        err = display_service_set_weather(cJSON_GetStringValue(city),
                                          cJSON_GetStringValue(summary), 0);
    } else if (strcmp(act, "set_todos") == 0) {
        cJSON *todos = cJSON_GetObjectItem(root, "todos");
        char todo_storage[MIMI_DISPLAY_MAX_TODOS][MIMI_DISPLAY_TODO_LEN];
        const char *todo_ptrs[MIMI_DISPLAY_MAX_TODOS];
        size_t todo_count = 0;
        if (cJSON_IsArray(todos)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, todos) {
                if (todo_count >= MIMI_DISPLAY_MAX_TODOS) break;
                const char *val = cJSON_GetStringValue(item);
                if (val) {
                    strncpy(todo_storage[todo_count], val, MIMI_DISPLAY_TODO_LEN - 1);
                    todo_storage[todo_count][MIMI_DISPLAY_TODO_LEN - 1] = '\0';
                    todo_ptrs[todo_count] = todo_storage[todo_count];
                    todo_count++;
                }
            }
        }
        err = display_service_set_todos(todo_ptrs, todo_count, 0);
    } else if (strcmp(act, "set_quote") == 0) {
        cJSON *quote = cJSON_GetObjectItem(root, "quote");
        err = display_service_set_quote(cJSON_GetStringValue(quote), 0);
    } else if (strcmp(act, "refresh") == 0) {
        err = display_service_request_refresh();
    } else if (strcmp(act, "show_image") == 0) {
        cJSON *b64_item = cJSON_GetObjectItem(root, "image_b64");
        const char *b64_str = cJSON_GetStringValue(b64_item);
        if (!b64_str) {
            send_display_ack(fd, act, "error", "missing image_b64");
            return;
        }
        size_t b64_len = strlen(b64_str);
        size_t expected_decoded = MIMI_DISPLAY_FB_BYTES;
        uint8_t *fb = malloc(expected_decoded);
        if (!fb) {
            send_display_ack(fd, act, "error", "out of memory");
            return;
        }
        size_t olen = 0;
        int ret = mbedtls_base64_decode(fb, expected_decoded, &olen,
                                        (const unsigned char *)b64_str, b64_len);
        if (ret != 0 || olen != expected_decoded) {
            free(fb);
            send_display_ack(fd, act, "error", "invalid image data");
            return;
        }
        err = display_service_show_image_frame(fb, olen);
        free(fb);
        if (err == ESP_OK) {
            send_display_ack(fd, act, "ok", NULL);
        } else {
            send_display_ack(fd, act, "error", esp_err_to_name(err));
        }
        return;
    } else {
        send_display_ack(fd, act, "error", "unknown action");
        return;
    }

    if (err == ESP_OK) {
        send_display_ack(fd, act, "ok", NULL);
    } else {
        send_display_ack(fd, act, "error", esp_err_to_name(err));
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* WebSocket handshake — register client */
        int fd = httpd_req_to_sockfd(req);
        add_client(fd);
        return ESP_OK;
    }

    /* Receive WebSocket frame */
    httpd_ws_frame_t ws_pkt = {0};
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Get frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.len == 0) return ESP_OK;

    ws_pkt.payload = calloc(1, ws_pkt.len + 1);
    if (!ws_pkt.payload) return ESP_ERR_NO_MEM;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(ws_pkt.payload);
        return ret;
    }

    int fd = httpd_req_to_sockfd(req);
    ws_client_t *client = find_client_by_fd(fd);

    /* Parse JSON message */
    cJSON *root = cJSON_Parse((char *)ws_pkt.payload);
    free(ws_pkt.payload);

    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON from fd=%d", fd);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *content = cJSON_GetObjectItem(root, "content");

    if (type && cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "message") == 0
            && content && cJSON_IsString(content)) {

            /* Determine chat_id */
            const char *chat_id = client ? client->chat_id : "ws_unknown";
            cJSON *cid = cJSON_GetObjectItem(root, "chat_id");
            if (cid && cJSON_IsString(cid)) {
                chat_id = cid->valuestring;
                /* Update client's chat_id if provided */
                if (client) {
                    strncpy(client->chat_id, chat_id, sizeof(client->chat_id) - 1);
                }
            }

            ESP_LOGI(TAG, "WS message from %s: %.40s...", chat_id, content->valuestring);

            /* Push to inbound bus */
            mimi_msg_t msg = {0};
            strncpy(msg.channel, MIMI_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(content->valuestring);
            if (msg.content) {
                message_bus_push_inbound(&msg);
            }
        } else if (strcmp(type->valuestring, "display") == 0) {
            int fd = httpd_req_to_sockfd(req);
            handle_display_message(fd, root);
        } else if (strcmp(type->valuestring, "file") == 0) {
            int fd = httpd_req_to_sockfd(req);
            handle_file_message(fd, root);
        } else if (strcmp(type->valuestring, "gpio") == 0) {
            int fd = httpd_req_to_sockfd(req);
            handle_gpio_message(fd, root);
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_WS_PORT;
    config.ctrl_port = MIMI_WS_PORT + 1;
    config.max_open_sockets = MIMI_WS_MAX_CLIENTS;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register WebSocket URI */
    httpd_uri_t ws_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    /* Register WebUI static page URI */
    httpd_uri_t ui_uri = {
        .uri = "/ui",
        .method = HTTP_GET,
        .handler = webui_handler,
        .is_websocket = false,
    };
    httpd_register_uri_handler(s_server, &ui_uri);

    ESP_LOGI(TAG, "WebSocket server started on port %d", MIMI_WS_PORT);
    return ESP_OK;
}

esp_err_t ws_server_send(const char *chat_id, const char *text)
{
    if (!s_server) return ESP_ERR_INVALID_STATE;

    ws_client_t *client = find_client_by_chat_id(chat_id);
    if (!client) {
        ESP_LOGW(TAG, "No WS client with chat_id=%s", chat_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Build response JSON */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json_str) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json_str,
        .len = strlen(json_str),
    };

    esp_err_t ret = httpd_ws_send_frame_async(s_server, client->fd, &ws_pkt);
    free(json_str);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send to %s: %s", chat_id, esp_err_to_name(ret));
        remove_client(client->fd);
    }

    return ret;
}

esp_err_t ws_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "WebSocket server stopped");
    }
    return ESP_OK;
}
