#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "spod_c6";

#ifndef CONFIG_SPOD_C6_WIFI_SSID
#define CONFIG_SPOD_C6_WIFI_SSID ""
#endif

#ifndef CONFIG_SPOD_C6_WIFI_PASSWORD
#define CONFIG_SPOD_C6_WIFI_PASSWORD ""
#endif

#ifndef CONFIG_SPOD_C6_WIFI_CONNECT_TIMEOUT_MS
#define CONFIG_SPOD_C6_WIFI_CONNECT_TIMEOUT_MS 12000
#endif

#ifndef CONFIG_SPOD_C6_WIFI_MAX_RETRY
#define CONFIG_SPOD_C6_WIFI_MAX_RETRY 8
#endif

#ifndef CONFIG_SPOD_C6_HTTP_PORT
#define CONFIG_SPOD_C6_HTTP_PORT 80
#endif

#ifndef CONFIG_SPOD_C6_UART_PORT
#define CONFIG_SPOD_C6_UART_PORT 1
#endif

#ifndef CONFIG_SPOD_C6_UART_TX_GPIO
#define CONFIG_SPOD_C6_UART_TX_GPIO 16
#endif

#ifndef CONFIG_SPOD_C6_UART_RX_GPIO
#define CONFIG_SPOD_C6_UART_RX_GPIO 17
#endif

#ifndef CONFIG_SPOD_C6_UART_BAUDRATE
#define CONFIG_SPOD_C6_UART_BAUDRATE 921600
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define UART_TX_BUF_SIZE      4096
#define UART_LOCK_TIMEOUT_MS  2000
#define UPLOAD_RECV_BUF_SIZE  2048

#define SPOD_PKT_MAGIC        0x31504453u /* "SPD1" */
#define SPOD_PKT_VERSION      1u

typedef enum {
    SPOD_PKT_HELLO = 1,
    SPOD_PKT_BEGIN = 2,
    SPOD_PKT_CHUNK = 3,
    SPOD_PKT_END = 4,
    SPOD_PKT_ABORT = 5,
    SPOD_PKT_FINALIZE = 6,
} spod_pkt_type_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t payload_len;
} spod_pkt_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t total_size;
    uint16_t story_len;
    uint16_t file_len;
} spod_pkt_begin_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t written_size;
} spod_pkt_end_payload_t;

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_wifi_netif = NULL;
static httpd_handle_t s_httpd = NULL;
static SemaphoreHandle_t s_uart_lock = NULL;
static bool s_wifi_started = false;
static bool s_wifi_connected = false;
static int s_wifi_retry_count = 0;
static char s_ip_addr[16] = {0};

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return (c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (c - 'A') + 10;
    }
    return -1;
}

static void url_decode_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
            continue;
        }
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_nibble(r[1]);
            int lo = hex_nibble(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static void sanitize_path_component(const char *in, char *out, size_t out_size, const char *fallback)
{
    if (!out || out_size == 0) {
        return;
    }

    size_t w = 0;
    if (in) {
        for (size_t i = 0; in[i] != '\0' && w + 1 < out_size; i++) {
            unsigned char c = (unsigned char)in[i];
            bool ok = isalnum(c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')';
            out[w++] = ok ? (char)c : '_';
        }
    }
    out[w] = '\0';

    while (w > 0 && out[w - 1] == ' ') {
        out[--w] = '\0';
    }

    size_t lead = 0;
    while (out[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        memmove(out, out + lead, strlen(out + lead) + 1);
    }

    if (strstr(out, "..")) {
        for (size_t i = 0; out[i] != '\0'; i++) {
            if (out[i] == '.') {
                out[i] = '_';
            }
        }
    }

    if (out[0] == '\0') {
        strlcpy(out, fallback ? fallback : "item", out_size);
    }
}

static esp_err_t bridge_send_packet_locked(spod_pkt_type_t type, const void *payload, size_t payload_len)
{
    if (payload_len > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uart_port_t uart_port = (uart_port_t)CONFIG_SPOD_C6_UART_PORT;
    spod_pkt_hdr_t hdr = {
        .magic = SPOD_PKT_MAGIC,
        .version = SPOD_PKT_VERSION,
        .type = (uint8_t)type,
        .reserved = 0,
        .payload_len = (uint32_t)payload_len,
    };

    int written = uart_write_bytes(uart_port, (const char *)&hdr, sizeof(hdr));
    if (written != (int)sizeof(hdr)) {
        ESP_LOGE(TAG, "UART header write failed (%d)", written);
        return ESP_FAIL;
    }

    if (payload_len > 0 && payload) {
        written = uart_write_bytes(uart_port, (const char *)payload, payload_len);
        if (written != (int)payload_len) {
            ESP_LOGE(TAG, "UART payload write failed (%d/%u)", written, (unsigned)payload_len);
            return ESP_FAIL;
        }
    }

    if (uart_wait_tx_done(uart_port, pdMS_TO_TICKS(1000)) != ESP_OK) {
        ESP_LOGW(TAG, "UART wait tx done timeout");
    }
    return ESP_OK;
}

static esp_err_t bridge_send_packet(spod_pkt_type_t type, const void *payload, size_t payload_len)
{
    if (!s_uart_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_uart_lock, pdMS_TO_TICKS(UART_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "UART lock timeout");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = bridge_send_packet_locked(type, payload, payload_len);
    xSemaphoreGive(s_uart_lock);
    return err;
}

static esp_err_t bridge_send_hello(void)
{
    static const char hello[] = "spod-c6-upload-v1";
    return bridge_send_packet(SPOD_PKT_HELLO, hello, sizeof(hello));
}

static esp_err_t bridge_send_begin(const char *story, const char *file_name, uint32_t total_size)
{
    if (!story || !file_name) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t story_len = strlen(story) + 1;
    size_t file_len = strlen(file_name) + 1;
    if (story_len > UINT16_MAX || file_len > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t payload_len = sizeof(spod_pkt_begin_payload_t) + story_len + file_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    spod_pkt_begin_payload_t *meta = (spod_pkt_begin_payload_t *)payload;
    meta->total_size = total_size;
    meta->story_len = (uint16_t)story_len;
    meta->file_len = (uint16_t)file_len;
    memcpy(payload + sizeof(*meta), story, story_len);
    memcpy(payload + sizeof(*meta) + story_len, file_name, file_len);

    esp_err_t err = bridge_send_packet(SPOD_PKT_BEGIN, payload, payload_len);
    free(payload);
    return err;
}

static esp_err_t bridge_send_chunk(const uint8_t *data, size_t len)
{
    return bridge_send_packet(SPOD_PKT_CHUNK, data, len);
}

static esp_err_t bridge_send_end(uint32_t written_size)
{
    const spod_pkt_end_payload_t end = {
        .written_size = written_size,
    };
    return bridge_send_packet(SPOD_PKT_END, &end, sizeof(end));
}

static esp_err_t bridge_send_abort(const char *reason)
{
    const char *msg = reason ? reason : "abort";
    return bridge_send_packet(SPOD_PKT_ABORT, msg, strlen(msg) + 1);
}

static esp_err_t bridge_send_finalize(const char *story_or_null)
{
    const char *name = story_or_null ? story_or_null : "";
    return bridge_send_packet(SPOD_PKT_FINALIZE, name, strlen(name) + 1);
}

static esp_err_t uart_bridge_init(void)
{
    s_uart_lock = xSemaphoreCreateMutex();
    if (!s_uart_lock) {
        return ESP_ERR_NO_MEM;
    }

    const uart_port_t uart_port = (uart_port_t)CONFIG_SPOD_C6_UART_PORT;
    const uart_config_t cfg = {
        .baud_rate = CONFIG_SPOD_C6_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(uart_port, 256, UART_TX_BUF_SIZE, 0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(uart_port, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_port,
                                     CONFIG_SPOD_C6_UART_TX_GPIO,
                                     CONFIG_SPOD_C6_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");

    ESP_LOGI(TAG,
             "UART bridge ready (port=%d tx=%d rx=%d baud=%d)",
             CONFIG_SPOD_C6_UART_PORT,
             CONFIG_SPOD_C6_UART_TX_GPIO,
             CONFIG_SPOD_C6_UART_RX_GPIO,
             CONFIG_SPOD_C6_UART_BAUDRATE);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_wifi_retry_count = 0;
            (void)esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_wifi_connected = false;
            s_ip_addr[0] = '\0';
            if (s_wifi_started && s_wifi_retry_count < CONFIG_SPOD_C6_WIFI_MAX_RETRY) {
                s_wifi_retry_count++;
                ESP_LOGW(TAG,
                         "Wi-Fi disconnected, retry %d/%d",
                         s_wifi_retry_count,
                         CONFIG_SPOD_C6_WIFI_MAX_RETRY);
                (void)esp_wifi_connect();
            } else {
                if (s_wifi_event_group) {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t wifi_stack_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_wifi_netif) {
        s_wifi_netif = esp_netif_create_default_wifi_sta();
        if (!s_wifi_netif) {
            return ESP_FAIL;
        }
    }

    static bool wifi_init_done = false;
    static esp_event_handler_instance_t wifi_evt_any_id;
    static esp_event_handler_instance_t ip_evt_got_ip;
    if (!wifi_init_done) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT,
                                                                ESP_EVENT_ANY_ID,
                                                                &wifi_event_handler,
                                                                NULL,
                                                                &wifi_evt_any_id),
                            TAG,
                            "wifi evt reg");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT,
                                                                IP_EVENT_STA_GOT_IP,
                                                                &wifi_event_handler,
                                                                NULL,
                                                                &ip_evt_got_ip),
                            TAG,
                            "ip evt reg");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage");
        wifi_init_done = true;
    }

    return ESP_OK;
}

static esp_err_t wifi_connect_sta(void)
{
    if (CONFIG_SPOD_C6_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty. Set it in menuconfig (sPod C6 Upload Bridge).");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(wifi_stack_init(), TAG, "wifi_stack_init");
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_SPOD_C6_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_SPOD_C6_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.scan_method = WIFI_FAST_SCAN;
    wifi_cfg.sta.failure_retry_cnt = CONFIG_SPOD_C6_WIFI_MAX_RETRY;
    wifi_cfg.sta.threshold.authmode = (CONFIG_SPOD_C6_WIFI_PASSWORD[0] == '\0')
                                          ? WIFI_AUTH_OPEN
                                          : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    s_wifi_started = true;

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(CONFIG_SPOD_C6_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected, IP=%s", s_ip_addr[0] ? s_ip_addr : "n/a");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Wi-Fi connect timeout/fail");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>sPod C6 Upload</title></head>"
        "<body style='font-family:sans-serif;max-width:760px;margin:24px auto;padding:0 12px'>"
        "<h2>sPod Upload (via C6)</h2>"
        "<p>Story files are streamed to ESP32-P4 over UART.</p>"
        "<label>Story folder</label><br><input id='story' style='width:100%;padding:8px' placeholder='Alba ca Zapada'><br><br>"
        "<input id='files' type='file' multiple><br><br>"
        "<button onclick='uploadAll()' style='padding:10px 14px'>Upload</button>"
        "<pre id='log' style='margin-top:16px;background:#f4f4f4;padding:10px;min-height:200px;white-space:pre-wrap'></pre>"
        "<script>"
        "const log=(m)=>{const e=document.getElementById('log');e.textContent+=m+'\\n';};"
        "async function uploadAll(){"
        "const story=document.getElementById('story').value.trim();"
        "const files=[...document.getElementById('files').files];"
        "if(!story){log('Missing story folder');return;}"
        "if(files.length===0){log('No files selected');return;}"
        "for(const f of files){"
        "const url='/api/upload?story='+encodeURIComponent(story)+'&filename='+encodeURIComponent(f.name);"
        "log('Uploading '+f.name+' ...');"
        "const r=await fetch(url,{method:'POST',body:f});"
        "const t=await r.text();"
        "if(!r.ok){log('Failed '+f.name+' ('+r.status+'): '+t);return;}"
        "log(t);"
        "}"
        "const done=await fetch('/api/finalize?story='+encodeURIComponent(story),{method:'POST'});"
        "log('Finalize: '+await done.text());"
        "log('Done.');"
        "}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char resp[196];
    snprintf(resp,
             sizeof(resp),
             "{\"ok\":true,\"wifi_connected\":%s,\"ip\":\"%s\",\"uart_baud\":%d}",
             s_wifi_connected ? "true" : "false",
             s_ip_addr[0] ? s_ip_addr : "",
             CONFIG_SPOD_C6_UART_BAUDRATE);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    int qlen = httpd_req_get_url_query_len(req);
    if (qlen <= 0 || qlen >= 256) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Missing query", HTTPD_RESP_USE_STRLEN);
    }

    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad query", HTTPD_RESP_USE_STRLEN);
    }

    char story_raw[96] = {0};
    char file_raw[96] = {0};
    if (httpd_query_key_value(query, "story", story_raw, sizeof(story_raw)) != ESP_OK ||
        httpd_query_key_value(query, "filename", file_raw, sizeof(file_raw)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Need story and filename", HTTPD_RESP_USE_STRLEN);
    }

    url_decode_inplace(story_raw);
    url_decode_inplace(file_raw);

    char story_name[96];
    char file_name[96];
    sanitize_path_component(story_raw, story_name, sizeof(story_name), "story");
    sanitize_path_component(file_raw, file_name, sizeof(file_name), "track.bin");

    const uint32_t total_size = (uint32_t)req->content_len;
    esp_err_t err = bridge_send_begin(story_name, file_name, total_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge_send_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Bridge begin failed", HTTPD_RESP_USE_STRLEN);
    }

    int remaining = req->content_len;
    uint8_t buf[UPLOAD_RECV_BUF_SIZE];
    uint32_t written = 0;

    while (remaining > 0) {
        int chunk = remaining;
        if (chunk > (int)sizeof(buf)) {
            chunk = (int)sizeof(buf);
        }

        int received = httpd_req_recv(req, (char *)buf, chunk);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "httpd_req_recv failed (%d)", received);
            break;
        }

        err = bridge_send_chunk(buf, (size_t)received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bridge_send_chunk failed: %s", esp_err_to_name(err));
            break;
        }

        written += (uint32_t)received;
        remaining -= received;
    }

    if (err == ESP_OK) {
        err = bridge_send_end(written);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bridge_send_end failed: %s", esp_err_to_name(err));
        }
    } else {
        (void)bridge_send_abort("upload_failed");
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Bridge upload failed", HTTPD_RESP_USE_STRLEN);
    }

    char resp[256];
    snprintf(resp,
             sizeof(resp),
             "{\"ok\":true,\"story\":\"%s\",\"file\":\"%s\",\"bytes\":%u}",
             story_name,
             file_name,
             (unsigned)written);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t finalize_post_handler(httpd_req_t *req)
{
    char story_raw[96] = {0};
    char story_name[96] = {0};
    int qlen = httpd_req_get_url_query_len(req);
    if (qlen > 0 && qlen < 256) {
        char query[256];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "story", story_raw, sizeof(story_raw)) == ESP_OK) {
            url_decode_inplace(story_raw);
            sanitize_path_component(story_raw, story_name, sizeof(story_name), "story");
        }
    }

    esp_err_t err = bridge_send_finalize(story_name[0] ? story_name : NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge_send_finalize failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Bridge finalize failed", HTTPD_RESP_USE_STRLEN);
    }

    char resp[140];
    snprintf(resp,
             sizeof(resp),
             "{\"ok\":true,\"story\":\"%s\"}",
             story_name);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    if (s_httpd) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_SPOD_C6_HTTP_PORT;
    cfg.ctrl_port = (cfg.server_port == 65535) ? 32768 : (cfg.server_port + 1);
    cfg.max_open_sockets = 4;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd_start");

    const httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_upload = {
        .uri = "/api/upload",
        .method = HTTP_POST,
        .handler = upload_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t uri_finalize = {
        .uri = "/api/finalize",
        .method = HTTP_POST,
        .handler = finalize_post_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(s_httpd, &uri_index);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_httpd, &uri_status);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_httpd, &uri_upload);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_httpd, &uri_finalize);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP uri register failed: %s", esp_err_to_name(err));
        httpd_stop(s_httpd);
        s_httpd = NULL;
        return err;
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "sPod C6 upload bridge boot");

    if (uart_bridge_init() != ESP_OK) {
        ESP_LOGE(TAG, "UART bridge init failed");
        return;
    }

    if (bridge_send_hello() != ESP_OK) {
        ESP_LOGW(TAG, "HELLO frame to P4 failed");
    }

    esp_err_t err = wifi_connect_sta();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi not started. Configure menuconfig and reboot.");
        return;
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return;
    }

    if (CONFIG_SPOD_C6_HTTP_PORT == 80) {
        ESP_LOGI(TAG, "Upload UI: http://%s", s_ip_addr);
    } else {
        ESP_LOGI(TAG, "Upload UI: http://%s:%d", s_ip_addr, CONFIG_SPOD_C6_HTTP_PORT);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
