#include "ws_server.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "gateway/webui_display.h"
#include "display/display_service.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

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
