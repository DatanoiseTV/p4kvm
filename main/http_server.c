/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#if CONFIG_P4KVM_AUTH_ENABLE
#include "mbedtls/base64.h"
#endif

#include "atx_ctrl.h"
#include "jpeg_frame.h"
#include "usb_hid.h"
#include "video_mode.h"
#include "video_stats.h"
#include "wireguard_net.h"

static const char *TAG = "p4kvm";

/**
 * Disable Nagle on a session socket. The MJPEG stream sends header + JPEG +
 * trailer as separate chunks per frame; with Nagle the small trailing chunks
 * sit in the stack until the previous segment is ACKed, adding up to an RTT
 * of latency per frame. HID input on /ws wants the same treatment.
 */
static void sock_set_nodelay(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
            ESP_LOGW(TAG, "TCP_NODELAY fd %d: errno %d", fd, errno);
        }
    }
}

#if CONFIG_P4KVM_AUTH_ENABLE
/**
 * HTTP Basic auth. Base64 credentials over plain HTTP are an access hurdle,
 * not transport security - the README still mandates a VPN.
 */
static bool auth_ok(httpd_req_t *req)
{
    static const char pass[] = CONFIG_P4KVM_AUTH_PASS;
    if (pass[0] == '\0') {
        return true; /* Half-configured build: fail open rather than brick access. */
    }
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncasecmp(hdr, "Basic ", 6) != 0) {
        return false;
    }
    unsigned char decoded[128];
    size_t dlen = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &dlen, (const unsigned char *)hdr + 6,
                              strlen(hdr + 6)) != 0) {
        return false;
    }
    decoded[dlen] = '\0';
    char expected[160];
    int n = snprintf(expected, sizeof(expected), "%s:%s", CONFIG_P4KVM_AUTH_USER, pass);
    if (n <= 0 || n >= (int)sizeof(expected)) {
        return false;
    }
    /* Constant-time over the expected length; length mismatch folded into the accumulator. */
    unsigned char diff = (dlen == (size_t)n) ? 0u : 1u;
    for (int i = 0; i < n; i++) {
        unsigned char b = ((size_t)i < dlen) ? decoded[i] : 0u;
        diff |= (unsigned char)(expected[i] ^ b);
    }
    return diff == 0u;
}

static esp_err_t auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"p4kvm\"");
    return httpd_resp_send(req, "unauthorized", HTTPD_RESP_USE_STRLEN);
}

#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#error "P4KVM_AUTH_ENABLE needs CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT (see sdkconfig.defaults)"
#endif

/**
 * esp_http_server completes the 101 WebSocket upgrade BEFORE invoking the URI
 * handler (httpd_uri.c), so auth inside the handler would run too late - an
 * unauthenticated client would already hold an upgraded socket. This callback
 * runs before the handshake; non-OK drops the connection without upgrading.
 */
static esp_err_t ws_auth_pre_handshake(httpd_req_t *req)
{
    return auth_ok(req) ? ESP_OK : ESP_FAIL;
}

#define AUTH_GATE(req)                                                                                                \
    do {                                                                                                              \
        if (!auth_ok(req)) {                                                                                          \
            return auth_reject(req);                                                                                  \
        }                                                                                                             \
    } while (0)
#else
#define AUTH_GATE(req) do { } while (0)
#endif

static void stream_release_slot_ref(int slot)
{
    if (slot < 0 || slot >= JPEG_SLOT_COUNT) {
        return;
    }
    if (xSemaphoreTake(g_jpeg_frame.mutex, portMAX_DELAY) == pdTRUE) {
        if (g_jpeg_frame.slot_ref[slot] > 0) {
            g_jpeg_frame.slot_ref[slot]--;
        }
        xSemaphoreGive(g_jpeg_frame.mutex);
    }
}

/* Long MJPEG response must not run on the httpd select() thread; see httpd_req_async_handler_begin(). */
#define STREAM_WORKER_STACK (12 * 1024)
#define STREAM_WORKER_PRIO (tskIDLE_PRIORITY + 5)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const char favicon_ico_end[] asm("_binary_favicon_ico_end");

static SemaphoreHandle_t s_ws_mu;
static int s_ws_fd = -1;

static void http_sess_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    if (!s_ws_mu) {
        return;
    }
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (sockfd == s_ws_fd) {
        s_ws_fd = -1;
    }
    xSemaphoreGive(s_ws_mu);
}

static void ws_take_session(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (s_ws_fd >= 0 && s_ws_fd != fd) {
        httpd_sess_trigger_close(req->handle, s_ws_fd);
    }
    s_ws_fd = fd;
    xSemaphoreGive(s_ws_mu);
}

static esp_err_t ws_input_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Auth already enforced by ws_auth_pre_handshake (the upgrade happens
         * before this handler runs, so a gate here would be too late). */
        sock_set_nodelay(req);
        ws_take_session(req);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "ws frame len query: %s", esp_err_to_name(ret));
        return ret;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        return ESP_OK;
    }

    uint8_t buf[32];
    if (pkt.len > sizeof(buf)) {
        ESP_LOGW(TAG, "ws frame too large %zu", (size_t)pkt.len);
        return ESP_OK;
    }
    if (pkt.len) {
        memset(buf, 0, sizeof(buf));
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_OK;
    }
    int my_fd = httpd_req_to_sockfd(req);
    bool ours;
    if (s_ws_fd == my_fd) {
        /* Already the registered owner. */
        ours = true;
    } else if (s_ws_fd < 0) {
        /* No owner registered (handshake's s_ws_fd was cleared by a spurious
         * close_fn, e.g. esp_http_server's internal session recycling).
         * Lazily claim this connection so HID input is not lost. */
        s_ws_fd = my_fd;
        ours = true;
    } else {
        /* A different client is the active owner, keep single-client enforcement. */
        ours = false;
    }
    xSemaphoreGive(s_ws_mu);

    if (!ours) {
        return ESP_OK;
    }

    if (pkt.len >= 8 && buf[0] == 0x01) {
        uint8_t buttons = buf[1];
        int8_t wheel = (int8_t)buf[6];
        bool relative = buf[7] != 0;
        if (relative) {
            int16_t dx = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
            int16_t dy = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
            usb_hid_mouse_rel(buttons, dx, dy, wheel);
        } else {
            uint16_t x = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
            uint16_t y = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
            usb_hid_mouse(buttons, x, y, wheel);
        }
    } else if (pkt.len >= 8 && buf[0] == 0x02) {
        usb_hid_keyboard(buf[1], &buf[2]);
    }

    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, len);
}

static esp_err_t favicon_get(httpd_req_t *req)
{
    const size_t len = (size_t)(favicon_ico_end - favicon_ico_start);
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, favicon_ico_start, len);
}

#define P4KVM_VERSION "0.2.0"

/** Append ,"key":"a.b.c.d" for a netif's IP if the interface exists and has one. */
static int stats_append_ip(char *dst, size_t cap, const char *key, const char *ifkey)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        return 0;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return 0;
    }
    return snprintf(dst, cap, ",\"%s\":\"" IPSTR "\"", key, IP2STR(&ip.ip));
}

/** GET /stats: pipeline counters as JSON for the UI diagnostics panel and A/B tuning. */
static esp_err_t stats_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char ips[80] = "";
    int off = stats_append_ip(ips, sizeof(ips), "ip_eth", "ETH_DEF");
    if (off >= 0 && (size_t)off < sizeof(ips)) {
        stats_append_ip(ips + off, sizeof(ips) - (size_t)off, "ip_wifi", "WIFI_STA_DEF");
    }

    char body[768];
    uint32_t cap_x10 = g_video_stats.cap_fps_x10;
    uint32_t enc_x10 = g_video_stats.enc_fps_x10;
    int n = snprintf(body, sizeof(body),
                     "{\"version\":\"" P4KVM_VERSION "\",\"pipeline\":\"%s\","
                     "\"hostname\":\"" CONFIG_P4KVM_MDNS_HOSTNAME "\","
                     "\"mode\":\"%s\",\"width\":%lu,\"height\":%lu,"
                     "\"cap_fps\":%u.%u,\"enc_fps\":%u.%u,"
                     "\"bs_us\":%u,\"enc_us\":%u,\"jpeg_bytes\":%u,\"quality\":%u,"
                     "\"clients\":%d,\"hdmi_locked\":%s,\"sys_status\":%u,"
                     "\"cap_frames\":%u,\"enc_frames\":%u,\"enc_errors\":%u,\"recoveries\":%u,"
                     "\"atx_power\":%s,\"atx_reset\":%s,\"usb_hid\":%s,\"wg\":\"%s\"%s,"
                     "\"uptime_s\":%lld,\"heap_free\":%u,\"psram_free\":%u}",
                     video_stats_pipeline_name(), video_mode_name(), (unsigned long)video_mode_hres(),
                     (unsigned long)video_mode_vres(), (unsigned)(cap_x10 / 10u), (unsigned)(cap_x10 % 10u),
                     (unsigned)(enc_x10 / 10u), (unsigned)(enc_x10 % 10u), (unsigned)g_video_stats.bs_us,
                     (unsigned)g_video_stats.enc_us, (unsigned)g_video_stats.jpeg_bytes,
                     (unsigned)g_jpeg_frame.jpeg_quality, jpeg_frame_stream_clients(),
                     g_video_stats.hdmi_locked ? "true" : "false", (unsigned)g_video_stats.sys_status,
                     (unsigned)g_video_stats.cap_frames, (unsigned)g_video_stats.enc_frames,
                     (unsigned)g_video_stats.enc_errors, (unsigned)g_video_stats.recoveries,
                     atx_ctrl_power_available() ? "true" : "false", atx_ctrl_reset_available() ? "true" : "false",
                     usb_hid_ready() ? "true" : "false", wireguard_net_status_str(), ips,
                     (long long)(esp_timer_get_time() / 1000000),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (n <= 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stats");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (size_t)n);
}

/** POST /atx?op=power|power_hold|reset - front-panel button press. */
static esp_err_t atx_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    char query[64];
    char op[24] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "op", op, sizeof(op)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing op");
    }
    atx_op_t kind;
    if (strcmp(op, "power") == 0) {
        kind = ATX_OP_POWER_TAP;
    } else if (strcmp(op, "power_hold") == 0) {
        kind = ATX_OP_POWER_HOLD;
    } else if (strcmp(op, "reset") == 0) {
        kind = ATX_OP_RESET_TAP;
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown op");
    }
    esp_err_t er = atx_ctrl_press(kind);
    if (er == ESP_ERR_NOT_SUPPORTED) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "ATX GPIO not configured");
    }
    if (er == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_custom_err(req, "409 Conflict", "press in progress");
    }
    if (er != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(er));
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok\n");
}

/** POST /video-mode?mode=720p60|1080p30 - persist to NVS and restart the device. */
static esp_err_t video_mode_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    char query[48];
    char mode[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "mode", mode, sizeof(mode)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode");
    }
    esp_err_t er = video_mode_set_and_reboot(mode);
    if (er == ESP_ERR_INVALID_ARG) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown mode (720p60|1080p30)");
    }
    if (er != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(er));
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "restarting\n");
}

/** GET /jpeg-quality optional query `q=1..100` sets quality; response body is current quality (text/plain). */
static esp_err_t jpeg_quality_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) {
            int q = atoi(val);
            if (q >= 1 && q <= 100) {
                g_jpeg_frame.jpeg_quality = (uint8_t)q;
                (void)jpeg_quality_save_to_nvs((uint8_t)q);
            }
        }
    }
    unsigned jq = (unsigned)g_jpeg_frame.jpeg_quality;
    if (jq < 1u) {
        jq = 1u;
    } else if (jq > 100u) {
        jq = 100u;
    }
    char resp[48];
    int n = snprintf(resp, sizeof(resp), "%u\n", jq);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "jpeg-quality");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, resp, (size_t)n);
}

/**
 * While httpd_req_async_handler_begin() is in effect, the session fd is not in the server's select()
 * set, so disconnects are invisible until we send or call httpd_req_async_handler_complete(), that
 * can exhaust session slots (accept errno 23 / ENFILE). Peek the TCP socket from this task instead.
 */
static bool stream_peer_disconnected(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return true;
    }
    unsigned char b;
    int n = recv(fd, &b, 1, MSG_DONTWAIT | MSG_PEEK);
    if (n == 0) {
        return true;
    }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return true;
    }
    return false;
}

static void stream_worker_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char hdr[96];

    sock_set_nodelay(req);
    jpeg_frame_stream_enter();
    /* Wait for the next camera frame after connect, avoids replaying one stale JPEG in a tight loop. */
    uint32_t last_seq = g_jpeg_frame.frame_seq;

    while (1) {
        bool stop = false;
        while (g_jpeg_frame.frame_seq == last_seq) {
            if (!g_jpeg_frame.frame_ready_sem) {
                vTaskDelay(pdMS_TO_TICKS(5));
                if (stream_peer_disconnected(req)) {
                    stop = true;
                    break;
                }
                continue;
            }
            if (xSemaphoreTake(g_jpeg_frame.frame_ready_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
                if (stream_peer_disconnected(req)) {
                    stop = true;
                    break;
                }
                continue;
            }
            while (xSemaphoreTake(g_jpeg_frame.frame_ready_sem, 0) == pdTRUE) {
                /* Coalesce bursty encoder completions, multipart sends latest frame only. */
            }
        }
        if (stop) {
            break;
        }

        if (xSemaphoreTake(g_jpeg_frame.xmit_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        size_t copy_len = 0;
        uint32_t seq_snap = last_seq;
        int slot = -1;
        if (xSemaphoreTake(g_jpeg_frame.mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        int f = g_jpeg_frame.front_idx;
        seq_snap = g_jpeg_frame.frame_seq;
        if (f >= 0 && f < JPEG_SLOT_COUNT && g_jpeg_frame.jpeg_buf[f]) {
            copy_len = g_jpeg_frame.jpeg_len[f];
            if (copy_len > 0 && copy_len <= g_jpeg_frame.jpeg_cap) {
                slot = f;
                g_jpeg_frame.slot_ref[slot]++;
            } else {
                copy_len = 0;
            }
        } else {
            copy_len = 0;
        }
        xSemaphoreGive(g_jpeg_frame.mutex);
        if (copy_len == 0 || slot < 0) {
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            last_seq = seq_snap;
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int hl = snprintf(hdr, sizeof(hdr),
                          "--frame\r\n"
                          "Content-Type: image/jpeg\r\n"
                          "Content-Length: %zu\r\n"
                          "\r\n",
                          copy_len);
        if (hl <= 0 || hl >= (int)sizeof(hdr)) {
            stream_release_slot_ref(slot);
            xSemaphoreGive(g_jpeg_frame.xmit_mutex);
            last_seq = seq_snap;
            if (stream_peer_disconnected(req)) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_err_t se = httpd_resp_send_chunk(req, hdr, hl);
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, (const char *)g_jpeg_frame.jpeg_buf[slot], copy_len);
        }
        if (se == ESP_OK) {
            se = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        stream_release_slot_ref(slot);
        xSemaphoreGive(g_jpeg_frame.xmit_mutex);

        if (se != ESP_OK) {
            ESP_LOGD(TAG, "stream end %s", esp_err_to_name(se));
            break;
        }
        last_seq = seq_snap;
    }
    jpeg_frame_stream_leave();
    httpd_resp_sendstr_chunk(req, NULL);
    if (httpd_req_async_handler_complete(req) != ESP_OK) {
        ESP_LOGW(TAG, "stream async complete failed");
    }
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    if (!g_jpeg_frame.jpeg_buf[0] || !g_jpeg_frame.jpeg_buf[1] || !g_jpeg_frame.jpeg_buf[2] ||
        !g_jpeg_frame.xmit_mutex || !g_jpeg_frame.frame_ready_sem) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera starting");
    }
    /* Browsers will often show only the first JPEG unless the stream is explicitly uncached. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    /* Hint for reverse proxies (harmless if unused). */
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    if (res != ESP_OK) {
        return res;
    }

    httpd_req_t *async_req = NULL;
    res = httpd_req_async_handler_begin(req, &async_req);
    if (res != ESP_OK) {
        ESP_LOGW(TAG, "stream async begin: %s", esp_err_to_name(res));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream busy");
    }

    BaseType_t created =
        xTaskCreate(stream_worker_task, "p4kvm_stream", STREAM_WORKER_STACK, async_req, STREAM_WORKER_PRIO, NULL);
    if (created != pdPASS) {
        httpd_req_async_handler_complete(async_req);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "stream task");
    }
    return ESP_OK;
}

httpd_handle_t http_server_start(void)
{
    if (!s_ws_mu) {
        s_ws_mu = xSemaphoreCreateMutex();
        if (!s_ws_mu) {
            ESP_LOGE(TAG, "ws mutex");
            return NULL;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Browsers send >1 KiB of headers; Vite dev proxy forwards them. Default 1024 → 431. */
    cfg.max_req_hdr_len = 8192;
    cfg.server_port = 80;
    cfg.stack_size = 20 * 1024;
    /* Prefer draining TCP slightly above capture so multipart frames reach the browser. */
    cfg.task_priority = tskIDLE_PRIORITY + 6;
    cfg.send_wait_timeout = 30;
    cfg.keep_alive_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.close_fn = http_sess_close_cb;
    /* Single-user KVM: do not evict long-lived /stream when /ws or /jpeg-quality connects */
    cfg.lru_purge_enable = false;
    cfg.max_open_sockets = 12;
    cfg.max_uri_handlers = 12;

    httpd_handle_t h = NULL;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start");
        return NULL;
    }

    httpd_uri_t u_root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_register_uri_handler(h, &u_root);
    httpd_uri_t u_favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
    httpd_register_uri_handler(h, &u_favicon);
    httpd_uri_t u_stream = {.uri = "/stream", .method = HTTP_GET, .handler = stream_get};
    httpd_register_uri_handler(h, &u_stream);
    httpd_uri_t u_jpeg_q = {.uri = "/jpeg-quality", .method = HTTP_GET, .handler = jpeg_quality_get};
    httpd_register_uri_handler(h, &u_jpeg_q);
    httpd_uri_t u_stats = {.uri = "/stats", .method = HTTP_GET, .handler = stats_get};
    httpd_register_uri_handler(h, &u_stats);
    httpd_uri_t u_atx = {.uri = "/atx", .method = HTTP_POST, .handler = atx_post};
    httpd_register_uri_handler(h, &u_atx);
    httpd_uri_t u_vmode = {.uri = "/video-mode", .method = HTTP_POST, .handler = video_mode_post};
    httpd_register_uri_handler(h, &u_vmode);
    httpd_uri_t u_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_input_handler,
        .user_ctx = NULL,
        .is_websocket = true,
#if CONFIG_P4KVM_AUTH_ENABLE
        .ws_pre_handshake_cb = ws_auth_pre_handshake,
#endif
    };
    httpd_register_uri_handler(h, &u_ws);
    return h;
}
