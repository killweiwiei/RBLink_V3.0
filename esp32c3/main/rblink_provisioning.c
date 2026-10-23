#include "rblink_provisioning.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

#include "rblink_link_protocol.h"

#define RBLINK_NVS_NAMESPACE       "rblink_wifi"
#define RBLINK_NVS_SSID            "ssid"
#define RBLINK_NVS_PASSWORD        "password"
#define RBLINK_HTTP_BODY_SIZE      256U
#define RBLINK_WIFI_CHANNEL        1U
#define RBLINK_MAX_AP_CLIENTS      4U

static const char *TAG = "rblink_prov";
static esp_netif_t *s_sta_netif;
static volatile bool s_sta_connected;
static volatile bool s_ap_active;
static volatile bool s_sta_configured;
static volatile uint8_t s_sta_disconnect_reason;
static volatile int8_t s_sta_disconnect_rssi;
static rblink_web_debug_exchange_t s_debug_exchange;
extern const uint8_t rb_logo_start[] asm("_binary_rb_logo_png_start");
extern const uint8_t rb_logo_end[] asm("_binary_rb_logo_png_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static bool rblink_credentials_valid(const char *ssid, const char *password)
{
    size_t ssid_length = strlen(ssid);
    size_t password_length = strlen(password);

    return (ssid_length >= 1U) && (ssid_length <= 32U) &&
           ((password_length == 0U) ||
            ((password_length >= 8U) && (password_length <= 63U)));
}

static bool rblink_credentials_load(char *ssid, size_t ssid_size,
                                    char *password, size_t password_size)
{
    nvs_handle_t handle = 0;
    size_t stored_ssid_size = ssid_size;
    size_t stored_password_size = password_size;

    if (nvs_open(RBLINK_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t ssid_result = nvs_get_str(handle, RBLINK_NVS_SSID, ssid,
                                        &stored_ssid_size);
    esp_err_t password_result = nvs_get_str(handle, RBLINK_NVS_PASSWORD,
                                            password, &stored_password_size);
    nvs_close(handle);
    return (ssid_result == ESP_OK) && (password_result == ESP_OK) &&
           rblink_credentials_valid(ssid, password);
}

static esp_err_t rblink_credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(RBLINK_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (result == ESP_OK) {
        result = nvs_set_str(handle, RBLINK_NVS_SSID, ssid);
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, RBLINK_NVS_PASSWORD, password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return result;
}

static esp_err_t rblink_credentials_clear(void)
{
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(RBLINK_NVS_NAMESPACE, NVS_READWRITE, &handle);

    if (result == ESP_OK) {
        result = nvs_erase_all(handle);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (handle != 0U) {
        nvs_close(handle);
    }
    return result;
}

static esp_err_t rblink_wifi_apply(const char *ssid, const char *password)
{
    wifi_config_t config = {0};

    if (!rblink_credentials_valid(ssid, password)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = strlen(password) == 0U ?
                                    WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "failed to set STA configuration");
    /* Persist only a configuration accepted by the Wi-Fi driver. This avoids
       reboot loops around credentials that failed validation at runtime. */
    ESP_RETURN_ON_ERROR(rblink_credentials_save(ssid, password), TAG,
                        "failed to save Wi-Fi credentials");
    s_sta_configured = true;
    s_sta_disconnect_reason = 0U;
    s_sta_disconnect_rssi = 0;
    s_sta_connected = false;
    esp_err_t disconnect_result = esp_wifi_disconnect();
    if (disconnect_result == ESP_ERR_WIFI_NOT_CONNECT) {
        return esp_wifi_connect();
    }
    /* WIFI_EVENT_STA_DISCONNECTED performs the reconnect after an active
     * connection has closed. Returning now avoids racing two connect calls. */
    return disconnect_result;
}

static void rblink_wifi_event(void *argument, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)argument;
    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED)) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        s_sta_connected = false;
        if (disconnected != NULL) {
            s_sta_disconnect_reason = disconnected->reason;
            s_sta_disconnect_rssi = disconnected->rssi;
            ESP_LOGW(TAG, "station disconnected: reason=%u rssi=%d",
                     disconnected->reason, disconnected->rssi);
        }
        if (s_sta_configured) {
            (void)esp_wifi_connect();
        }
    } else if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        s_sta_connected = true;
        s_sta_disconnect_reason = 0U;
        s_sta_disconnect_rssi = 0;
        ESP_LOGI(TAG, "station connected");
    }
}

static void rblink_status_json(char *output, size_t output_size)
{
    char ssid[33] = {0};
    char password[64] = {0};
    char ap_ssid[33] = {0};
    wifi_config_t ap_config = {0};
    esp_netif_ip_info_t ip_info = {0};
    bool configured = rblink_credentials_load(ssid, sizeof(ssid), password,
                                               sizeof(password));
    cJSON *root = cJSON_CreateObject();

    (void)esp_wifi_get_config(WIFI_IF_AP, &ap_config);
    strlcpy(ap_ssid, (const char *)ap_config.ap.ssid, sizeof(ap_ssid));
    if (s_sta_netif != NULL) {
        (void)esp_netif_get_ip_info(s_sta_netif, &ip_info);
    }
    if (root == NULL) {
        strlcpy(output, "{\"error\":\"no_memory\"}", output_size);
        return;
    }
    cJSON_AddBoolToObject(root, "configured", configured);
    cJSON_AddBoolToObject(root, "connected", s_sta_connected);
    cJSON_AddBoolToObject(root, "ap_active", s_ap_active);
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddStringToObject(root, "sta_ssid", configured ? ssid : "");
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    cJSON_AddStringToObject(root, "ip", ip);
    if (!cJSON_PrintPreallocated(root, output, (int)output_size, false)) {
        strlcpy(output, "{\"error\":\"status_too_large\"}", output_size);
    }
    cJSON_Delete(root);
}

static esp_err_t rblink_http_root(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t rblink_http_debug_ws(httpd_req_t *request)
{
    httpd_ws_frame_t incoming = {0};
    httpd_ws_frame_t outgoing = {0};
    uint8_t *request_frame;
    uint8_t *response_frame;
    esp_err_t result;

    if (request->method == HTTP_GET) {
        return ESP_OK;
    }
    result = httpd_ws_recv_frame(request, &incoming, 0);
    if (result != ESP_OK) {
        return result;
    }
    if ((incoming.type != HTTPD_WS_TYPE_BINARY) ||
        (incoming.len != RBLINK_FRAME_SIZE) || (s_debug_exchange == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    request_frame = malloc(RBLINK_FRAME_SIZE);
    response_frame = malloc(RBLINK_FRAME_SIZE);
    if ((request_frame == NULL) || (response_frame == NULL)) {
        free(request_frame);
        free(response_frame);
        return ESP_ERR_NO_MEM;
    }
    incoming.payload = request_frame;
    result = httpd_ws_recv_frame(request, &incoming, RBLINK_FRAME_SIZE);
    if ((result == ESP_OK) && s_debug_exchange(request_frame, response_frame)) {
        outgoing.type = HTTPD_WS_TYPE_BINARY;
        outgoing.payload = response_frame;
        outgoing.len = RBLINK_FRAME_SIZE;
        result = httpd_ws_send_frame(request, &outgoing);
    } else if (result == ESP_OK) {
        static uint8_t busy[] = "RBLink wireless transport is busy or timed out";
        outgoing.type = HTTPD_WS_TYPE_TEXT;
        outgoing.payload = busy;
        outgoing.len = sizeof(busy) - 1U;
        result = httpd_ws_send_frame(request, &outgoing);
    }
    free(request_frame);
    free(response_frame);
    return result;
}

static esp_err_t rblink_http_logo(httpd_req_t *request)
{
    httpd_resp_set_type(request, "image/png");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(request, (const char *)rb_logo_start,
                           rb_logo_end - rb_logo_start);
}

static esp_err_t rblink_http_status(httpd_req_t *request)
{
    char response[256];
    rblink_status_json(response, sizeof(response));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t rblink_parse_and_apply_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *ssid;
    cJSON *password;
    esp_err_t result = ESP_ERR_INVALID_ARG;

    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    password = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(ssid) && cJSON_IsString(password)) {
        result = rblink_wifi_apply(ssid->valuestring, password->valuestring);
    }
    cJSON_Delete(root);
    return result;
}

static esp_err_t rblink_http_config(httpd_req_t *request)
{
    char body[RBLINK_HTTP_BODY_SIZE];
    size_t total = 0U;
    uint8_t timeout_count = 0U;

    struct sockaddr_storage peer = {0};
    socklen_t peer_size = sizeof(peer);
    int socket_fd = httpd_req_to_sockfd(request);
    if ((getpeername(socket_fd, (struct sockaddr *)&peer, &peer_size) != 0) ||
        (peer.ss_family != AF_INET) ||
        ((ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr) & UINT32_C(0xFFFFFF00)) !=
         UINT32_C(0xC0A80400))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "configuration is allowed only from the RBLink AP");
    }
    if ((request->content_len <= 0) ||
        (request->content_len >= (int)sizeof(body))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid request size");
    }
    /* HTTP bodies may arrive in several TCP segments. Read exactly
       content_len instead of parsing the first fragment as complete JSON. */
    while (total < (size_t)request->content_len) {
        int received = httpd_req_recv(request, body + total,
                                      request->content_len - total);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count < 3U) continue;
            return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT,
                                       "request body timeout");
        }
        if (received <= 0) {
            return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                       "request read failed");
        }
        total += (size_t)received;
    }
    body[total] = '\0';
    if (rblink_parse_and_apply_json(body) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "SSID must be 1-32 bytes; password must be empty or 8-63 bytes");
    }
    return httpd_resp_sendstr(request, "配置已保存，正在连接路由器。");
}

static esp_err_t rblink_http_clear(httpd_req_t *request)
{
    struct sockaddr_storage peer = {0};
    socklen_t peer_size = sizeof(peer);
    int socket_fd = httpd_req_to_sockfd(request);
    if ((getpeername(socket_fd, (struct sockaddr *)&peer, &peer_size) != 0) ||
        (peer.ss_family != AF_INET) ||
        ((ntohl(((struct sockaddr_in *)&peer)->sin_addr.s_addr) & UINT32_C(0xFFFFFF00)) !=
         UINT32_C(0xC0A80400))) {
        return httpd_resp_send_err(request, HTTPD_403_FORBIDDEN,
                                   "configuration is allowed only from the RBLink AP");
    }
    wifi_config_t empty = {0};
    s_sta_connected = false;
    s_sta_configured = false;
    (void)esp_wifi_disconnect();
    (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
    ESP_RETURN_ON_ERROR(rblink_credentials_clear(), TAG,
                        "failed to clear Wi-Fi credentials");
    return httpd_resp_sendstr(request, "Wi-Fi 配置已清除。");
}

static void rblink_http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET,
                              .handler = rblink_http_root};
    const httpd_uri_t logo = {.uri = "/logo.png", .method = HTTP_GET,
                              .handler = rblink_http_logo};
    const httpd_uri_t status = {.uri = "/api/status", .method = HTTP_GET,
                                .handler = rblink_http_status};
    const httpd_uri_t set_config = {.uri = "/api/config", .method = HTTP_POST,
                                    .handler = rblink_http_config};
    const httpd_uri_t clear_config = {.uri = "/api/config", .method = HTTP_DELETE,
                                      .handler = rblink_http_clear};
    const httpd_uri_t debug_ws = {.uri = "/ws/debug", .method = HTTP_GET,
                                  .handler = rblink_http_debug_ws,
                                  .is_websocket = true};

    config.max_uri_handlers = 6;
    config.stack_size = 6144;
    /* LWIP_MAX_SOCKETS is 8.  The HTTP server reserves three sockets
     * internally, so keep three client sockets and leave two for the
     * RBLink TCP listener and its active client. */
    config.max_open_sockets = 3;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &logo));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set_config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &clear_config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &debug_ws));
}

void rblink_provisioning_init(rblink_web_debug_exchange_t debug_exchange)
{
    uint8_t mac[6];
    char ap_ssid[33];
    char sta_ssid[33] = {0};
    char sta_password[64] = {0};
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_config = {0};
    wifi_config_t sta_config = {0};

    s_debug_exchange = debug_exchange;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if ((s_sta_netif == NULL) || (esp_netif_create_default_wifi_ap() == NULL)) {
        ESP_LOGE(TAG, "failed to create Wi-Fi interfaces");
        abort();
    }
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               rblink_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               rblink_wifi_event, NULL));

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X%02X",
             CONFIG_RBLINK_AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_RBLINK_AP_PASSWORD,
            sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = (uint8_t)strlen(ap_ssid);
    ap_config.ap.channel = RBLINK_WIFI_CHANNEL;
    ap_config.ap.max_connection = RBLINK_MAX_AP_CLIENTS;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.required = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_sta_configured = rblink_credentials_load(sta_ssid, sizeof(sta_ssid),
                                               sta_password,
                                               sizeof(sta_password));
    if (s_sta_configured) {
        strlcpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
        strlcpy((char *)sta_config.sta.password, sta_password,
                sizeof(sta_config.sta.password));
        sta_config.sta.threshold.authmode = strlen(sta_password) == 0U ?
                                            WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        sta_config.sta.pmf_cfg.capable = true;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    /* RBLink is externally powered while debugging. Disable modem sleep so
     * TCP/SPI command latency is deterministic instead of waiting for the
     * next Wi-Fi wake interval. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_ap_active = true;
    if (s_sta_configured) {
        (void)esp_wifi_connect();
    }
    rblink_http_start();

    ESP_LOGI(TAG, "AP %s ready; provisioning page http://192.168.4.1", ap_ssid);
}

bool rblink_provisioning_command(const uint8_t *request, uint16_t request_length,
                                 uint8_t *response, uint16_t *response_length)
{
    enum { COMMAND_STATUS = 0x10, COMMAND_SET = 0x11, COMMAND_CLEAR = 0x12 };
    char ssid[33] = {0};
    char password[64] = {0};
    esp_netif_ip_info_t ip_info = {0};

    if ((request == NULL) || (response == NULL) || (response_length == NULL) ||
        (request_length == 0U)) {
        return false;
    }
    response[0] = request[0];
    response[1] = 0U;
    if (request[0] == COMMAND_STATUS) {
        bool configured = rblink_credentials_load(ssid, sizeof(ssid), password,
                                                  sizeof(password));
        uint8_t ssid_length = configured ? (uint8_t)strlen(ssid) : 0U;
        if (s_sta_netif != NULL) {
            (void)esp_netif_get_ip_info(s_sta_netif, &ip_info);
        }
        response[2] = (configured ? 0x01U : 0U) |
                      (s_sta_connected ? 0x02U : 0U) |
                      (s_ap_active ? 0x04U : 0U);
        response[3] = ssid_length;
        memcpy(&response[4], ssid, ssid_length);
        memcpy(&response[4U + ssid_length], &ip_info.ip.addr, 4U);
        response[8U + ssid_length] = s_sta_disconnect_reason;
        response[9U + ssid_length] = (uint8_t)s_sta_disconnect_rssi;
        /* Optional trailing ESP software version. Older hosts ignore these
         * bytes; newer hosts can show both STM32 and ESP versions even while
         * commands are routed through USB. */
        response[10U + ssid_length] = 0U;
        response[11U + ssid_length] = 2U;
        response[12U + ssid_length] = 2U;
        *response_length = (uint16_t)(13U + ssid_length);
        return true;
    }
    if (request[0] == COMMAND_SET) {
        uint8_t ssid_length;
        uint8_t password_length;
        if (request_length < 3U) {
            response[1] = 1U;
        } else {
            ssid_length = request[1];
            password_length = request[2];
            if ((request_length != (uint16_t)(3U + ssid_length + password_length)) ||
                (ssid_length == 0U) || (ssid_length > 32U) ||
                !((password_length == 0U) ||
                  ((password_length >= 8U) && (password_length <= 63U)))) {
                response[1] = 1U;
            } else {
                memcpy(ssid, &request[3], ssid_length);
                memcpy(password, &request[3U + ssid_length], password_length);
                response[1] = rblink_wifi_apply(ssid, password) == ESP_OK ? 0U : 2U;
            }
        }
        *response_length = 2U;
        return true;
    }
    if (request[0] == COMMAND_CLEAR) {
        wifi_config_t empty = {0};
        s_sta_connected = false;
        s_sta_configured = false;
        (void)esp_wifi_disconnect();
        (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
        response[1] = rblink_credentials_clear() == ESP_OK ? 0U : 2U;
        *response_length = 2U;
        return true;
    }
    return false;
}

bool rblink_provisioning_sta_connected(void)
{
    return s_sta_connected;
}

bool rblink_provisioning_ap_active(void)
{
    return s_ap_active;
}
