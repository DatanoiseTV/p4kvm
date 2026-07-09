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

#include "cJSON.h"

#include "atx_ctrl.h"
#include "audio_stream.h"
#include "capture_h264.h"
#include "jpeg_frame.h"
#include "runtime_cfg.h"
#include "usb_hid.h"
#include "usb_msc.h"
#include "usb_serial.h"
#include "video_mode.h"
#include "video_stats.h"
#include "webrtc_kvm.h"
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
/* Socket of the active MJPEG stream (single-viewer KVM). Newest viewer wins: a
 * new /stream force-closes the previous stream's socket so its worker exits and
 * frees the socket immediately. A hard "one at a time" reject was worse - a
 * worker that never noticed its half-open peer would hold the slot forever,
 * locking everyone out and driving the browser into a reconnect storm that
 * exhausted the lwIP socket pool (accept -> ENFILE). s_ws_mu guards this too. */
static int s_stream_fd = -1;

static void http_sess_close_cb(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    audio_stream_on_sock_close(sockfd);
    usb_serial_on_sock_close(sockfd);
    if (!s_ws_mu) {
        return;
    }
    if (xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (sockfd == s_ws_fd) {
        s_ws_fd = -1;
    }
    if (sockfd == s_stream_fd) {
        s_stream_fd = -1;
    }
    xSemaphoreGive(s_ws_mu);
}

/* Register this connection as the active stream, force-closing the previous one
 * (newest viewer wins). Mirrors ws_take_session. */
static void stream_take_session(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (s_stream_fd >= 0 && s_stream_fd != fd) {
        httpd_sess_trigger_close(req->handle, s_stream_fd);
    }
    s_stream_fd = fd;
    xSemaphoreGive(s_ws_mu);
}

/* Release the active-stream slot if this fd still owns it (worker exit / start failure). */
static void stream_drop_session(int fd)
{
    if (!s_ws_mu || xSemaphoreTake(s_ws_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (s_stream_fd == fd) {
        s_stream_fd = -1;
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

    /* Same wire format as the WebRTC HID data channel (see usb_hid_dispatch_report). */
    usb_hid_dispatch_report(buf, pkt.len);

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
    uint32_t tx_x10 = g_video_stats.tx_fps_x10;
    int n = snprintf(body, sizeof(body),
                     "{\"version\":\"" P4KVM_VERSION "\",\"pipeline\":\"%s\","
                     "\"hostname\":\"" CONFIG_P4KVM_MDNS_HOSTNAME "\","
                     "\"mode\":\"%s\",\"width\":%lu,\"height\":%lu,"
                     "\"cap_fps\":%u.%u,\"enc_fps\":%u.%u,\"tx_fps\":%u.%u,"
                     "\"bs_us\":%u,\"enc_us\":%u,\"jpeg_bytes\":%u,\"quality\":%u,\"max_fps\":%u,"
                     "\"clients\":%d,\"hdmi_locked\":%s,\"sys_status\":%u,"
                     "\"cap_frames\":%u,\"enc_frames\":%u,\"enc_errors\":%u,\"recoveries\":%u,"
                     "\"atx_power\":%s,\"atx_reset\":%s,\"usb_hid\":%s,\"wg\":\"%s\",\"audio\":\"%s\","
                     "\"serial\":\"%s\"%s,\"webrtc\":\"%s\",\"h264_fps\":%u.%u,"
                     "\"uptime_s\":%lld,\"heap_free\":%u,\"psram_free\":%u}",
                     video_stats_pipeline_name(), video_mode_name(), (unsigned long)video_mode_hres(),
                     (unsigned long)video_mode_vres(), (unsigned)(cap_x10 / 10u), (unsigned)(cap_x10 % 10u),
                     (unsigned)(enc_x10 / 10u), (unsigned)(enc_x10 % 10u), (unsigned)(tx_x10 / 10u),
                     (unsigned)(tx_x10 % 10u), (unsigned)g_video_stats.bs_us,
                     (unsigned)g_video_stats.enc_us, (unsigned)g_video_stats.jpeg_bytes,
                     (unsigned)g_jpeg_frame.jpeg_quality, (unsigned)g_jpeg_frame.stream_max_fps,
                     jpeg_frame_stream_clients(),
                     g_video_stats.hdmi_locked ? "true" : "false", (unsigned)g_video_stats.sys_status,
                     (unsigned)g_video_stats.cap_frames, (unsigned)g_video_stats.enc_frames,
                     (unsigned)g_video_stats.enc_errors, (unsigned)g_video_stats.recoveries,
                     atx_ctrl_power_available() ? "true" : "false", atx_ctrl_reset_available() ? "true" : "false",
                     usb_hid_ready() ? "true" : "false", wireguard_net_status_str(), audio_stream_status_str(),
                     usb_serial_status_str(), ips, webrtc_kvm_state_str(),
                     (unsigned)(capture_h264_fps_x10() / 10u), (unsigned)(capture_h264_fps_x10() % 10u),
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

/** GET /media/status - virtual-media LUN state as JSON. */
static esp_err_t media_status_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char body[192];
    int n = snprintf(body, sizeof(body),
                     "{\"available\":%s,\"present\":%s,\"writable\":%s,"
                     "\"size_bytes\":%u,\"block_count\":%u,\"block_size\":%u,\"max_bytes\":%u}",
                     usb_msc_available() ? "true" : "false", usb_msc_medium_present() ? "true" : "false",
                     usb_msc_writable() ? "true" : "false", (unsigned)usb_msc_capacity_bytes(),
                     (unsigned)usb_msc_block_count(), (unsigned)usb_msc_block_size(),
                     (unsigned)(12u * 1024u * 1024u));
    if (n <= 0 || n >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, (size_t)n);
}

/**
 * POST /media/image?writable=0|1 - stream a raw .img/.iso into the PSRAM
 * ramdisk. Content-Length is the image size; the body is the raw bytes. On
 * success the host re-reads the new capacity (UNIT ATTENTION). Read-only by
 * default so a booting target cannot corrupt the image.
 */
static esp_err_t media_image_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    if (!usb_msc_available()) {
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", "MSC not ready");
    }
    int total = req->content_len;
    if (total <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    }

    bool writable = false;
    char query[64];
    char v[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "writable", v, sizeof(v)) == ESP_OK) {
        writable = (v[0] == '1' || v[0] == 't');
    }

    esp_err_t er = usb_msc_begin_image((uint32_t)total);
    if (er == ESP_ERR_INVALID_SIZE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too large (max 12 MiB PSRAM)");
    }
    if (er != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc");
    }

    /* Chunk straight from the socket into PSRAM; no full-image bounce buffer. */
    char buf[4096];
    uint32_t off = 0;
    while ((int)off < total) {
        int want = total - (int)off;
        if (want > (int)sizeof(buf)) {
            want = (int)sizeof(buf);
        }
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) {
            /* Truncated upload: leave the medium ejected rather than mount garbage. */
            usb_msc_eject_to_floppy();
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        }
        if (usb_msc_load(off, buf, (uint32_t)r) < 0) {
            usb_msc_eject_to_floppy();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "load");
        }
        off += (uint32_t)r;
    }

    usb_msc_commit_image(writable);
    httpd_resp_set_type(req, "application/json");
    char ok[96];
    int n = snprintf(ok, sizeof(ok), "{\"mounted\":true,\"size_bytes\":%u,\"writable\":%s}", (unsigned)off,
                     writable ? "true" : "false");
    return httpd_resp_send(req, ok, n > 0 ? (size_t)n : 0);
}

/** POST /media/eject - discard the mounted image, revert to the empty floppy. */
static esp_err_t media_eject_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    esp_err_t er = usb_msc_eject_to_floppy();
    if (er != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(er));
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok\n");
}

/* Kconfig fallbacks for /config reads; empty when the feature is compiled out. */
#if CONFIG_P4KVM_WIFI_ENABLE
#define CFG_DFLT_WIFI_SSID CONFIG_P4KVM_WIFI_SSID
#define CFG_DFLT_WIFI_PASS CONFIG_P4KVM_WIFI_PASSWORD
#else
#define CFG_DFLT_WIFI_SSID ""
#define CFG_DFLT_WIFI_PASS ""
#endif
#if CONFIG_P4KVM_WG_ENABLE
#define CFG_DFLT_WG_PRIV CONFIG_P4KVM_WG_PRIVATE_KEY
#define CFG_DFLT_WG_PUB CONFIG_P4KVM_WG_PEER_PUBLIC_KEY
#define CFG_DFLT_WG_PSK CONFIG_P4KVM_WG_PRESHARED_KEY
#define CFG_DFLT_WG_EP CONFIG_P4KVM_WG_ENDPOINT
#define CFG_DFLT_WG_PORT CONFIG_P4KVM_WG_PORT
#define CFG_DFLT_WG_IP CONFIG_P4KVM_WG_LOCAL_IP
#define CFG_DFLT_WG_MASK CONFIG_P4KVM_WG_LOCAL_NETMASK
#define CFG_DFLT_WG_KA CONFIG_P4KVM_WG_KEEPALIVE
#else
#define CFG_DFLT_WG_PRIV ""
#define CFG_DFLT_WG_PUB ""
#define CFG_DFLT_WG_PSK ""
#define CFG_DFLT_WG_EP ""
#define CFG_DFLT_WG_PORT 51820
#define CFG_DFLT_WG_IP "10.0.0.2"
#define CFG_DFLT_WG_MASK "255.255.255.0"
#define CFG_DFLT_WG_KA 25
#endif
#ifdef CONFIG_P4KVM_ATX_ACTIVE_HIGH
#define CFG_DFLT_ATX_LVL 1
#else
#define CFG_DFLT_ATX_LVL 0
#endif

/**
 * GET /config: current runtime settings as JSON. Secrets (WiFi password,
 * WireGuard private/preshared key) are never returned - only *_set flags,
 * so the page can show "configured" without exposing them.
 */
static esp_err_t config_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char ssid[33], pass[65], priv[64], pub[64], psk[64], ep[96], ip[20], mask[20];
    char turn_url[96], turn_secret[64];
    runtime_cfg_get_str(RT_KEY_TURN_URL, "", turn_url, sizeof(turn_url));
    runtime_cfg_get_str(RT_KEY_TURN_SECRET, "", turn_secret, sizeof(turn_secret));
    runtime_cfg_get_str(RT_KEY_WIFI_SSID, CFG_DFLT_WIFI_SSID, ssid, sizeof(ssid));
    runtime_cfg_get_str(RT_KEY_WIFI_PASS, CFG_DFLT_WIFI_PASS, pass, sizeof(pass));
    runtime_cfg_get_str(RT_KEY_WG_PRIV, CFG_DFLT_WG_PRIV, priv, sizeof(priv));
    runtime_cfg_get_str(RT_KEY_WG_PEER_PUB, CFG_DFLT_WG_PUB, pub, sizeof(pub));
    runtime_cfg_get_str(RT_KEY_WG_PSK, CFG_DFLT_WG_PSK, psk, sizeof(psk));
    runtime_cfg_get_str(RT_KEY_WG_ENDPOINT, CFG_DFLT_WG_EP, ep, sizeof(ep));
    runtime_cfg_get_str(RT_KEY_WG_LOCAL_IP, CFG_DFLT_WG_IP, ip, sizeof(ip));
    runtime_cfg_get_str(RT_KEY_WG_MASK, CFG_DFLT_WG_MASK, mask, sizeof(mask));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(root, "wifi_ssid", ssid);
    cJSON_AddBoolToObject(root, "wifi_pass_set", pass[0] != '\0');
    cJSON_AddBoolToObject(root, "wg_priv_set", priv[0] != '\0');
    cJSON_AddStringToObject(root, "wg_peer_pubkey", pub);
    cJSON_AddBoolToObject(root, "wg_psk_set", psk[0] != '\0');
    cJSON_AddStringToObject(root, "wg_endpoint", ep);
    cJSON_AddNumberToObject(root, "wg_port", runtime_cfg_get_i32(RT_KEY_WG_PORT, CFG_DFLT_WG_PORT));
    cJSON_AddStringToObject(root, "wg_local_ip", ip);
    cJSON_AddStringToObject(root, "wg_local_mask", mask);
    cJSON_AddNumberToObject(root, "wg_keepalive", runtime_cfg_get_i32(RT_KEY_WG_KEEPALIVE, CFG_DFLT_WG_KA));
    cJSON_AddNumberToObject(root, "atx_power_gpio", runtime_cfg_get_i32(RT_KEY_ATX_POWER, CONFIG_P4KVM_ATX_POWER_GPIO));
    cJSON_AddNumberToObject(root, "atx_reset_gpio", runtime_cfg_get_i32(RT_KEY_ATX_RESET, CONFIG_P4KVM_ATX_RESET_GPIO));
    cJSON_AddBoolToObject(root, "atx_active_high", runtime_cfg_get_i32(RT_KEY_ATX_ACTIVE_HIGH, CFG_DFLT_ATX_LVL) != 0);
    cJSON_AddStringToObject(root, "turn_url", turn_url);
    cJSON_AddBoolToObject(root, "turn_secret_set", turn_secret[0] != '\0');
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    esp_err_t er = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return er;
}

/** Store a string field if present in the JSON body. Empty string clears back to the Kconfig fallback path. */
static void config_take_str(cJSON *root, const char *json_key, const char *nvs_key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, json_key);
    if (cJSON_IsString(v)) {
        runtime_cfg_set_str(nvs_key, v->valuestring);
    }
}

static void config_take_i32(cJSON *root, const char *json_key, const char *nvs_key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, json_key);
    if (cJSON_IsNumber(v)) {
        runtime_cfg_set_i32(nvs_key, (int32_t)v->valuedouble);
    } else if (cJSON_IsBool(v)) {
        runtime_cfg_set_i32(nvs_key, cJSON_IsTrue(v) ? 1 : 0);
    }
}

/**
 * POST /config: JSON body with any subset of settings; applied on next boot.
 * `?reboot=1` restarts the device after saving (reuses the video-mode timer
 * path so the response flushes first).
 */
static esp_err_t config_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    char body[1024];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body size");
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        }
        got += r;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
    }
    config_take_str(root, "wifi_ssid", RT_KEY_WIFI_SSID);
    config_take_str(root, "wifi_pass", RT_KEY_WIFI_PASS);
    config_take_str(root, "wg_private_key", RT_KEY_WG_PRIV);
    config_take_str(root, "wg_peer_pubkey", RT_KEY_WG_PEER_PUB);
    config_take_str(root, "wg_psk", RT_KEY_WG_PSK);
    config_take_str(root, "wg_endpoint", RT_KEY_WG_ENDPOINT);
    config_take_i32(root, "wg_port", RT_KEY_WG_PORT);
    config_take_str(root, "wg_local_ip", RT_KEY_WG_LOCAL_IP);
    config_take_str(root, "wg_local_mask", RT_KEY_WG_MASK);
    config_take_i32(root, "wg_keepalive", RT_KEY_WG_KEEPALIVE);
    config_take_i32(root, "atx_power_gpio", RT_KEY_ATX_POWER);
    config_take_i32(root, "atx_reset_gpio", RT_KEY_ATX_RESET);
    config_take_i32(root, "atx_active_high", RT_KEY_ATX_ACTIVE_HIGH);
    config_take_str(root, "turn_url", RT_KEY_TURN_URL);
    config_take_str(root, "turn_secret", RT_KEY_TURN_SECRET);
    cJSON_Delete(root);

    char query[32];
    bool reboot = false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char rv[4];
        reboot = (httpd_query_key_value(query, "reboot", rv, sizeof(rv)) == ESP_OK && rv[0] == '1');
    }
    httpd_resp_set_type(req, "text/plain");
    esp_err_t er = httpd_resp_sendstr(req, reboot ? "saved, restarting\n" : "saved\n");
    if (reboot) {
        video_mode_schedule_restart();
    }
    return er;
}

#if CONFIG_P4KVM_WEBRTC_ENABLE
/**
 * POST /webrtc/offer - automatic WebRTC signaling over the device's own HTTP
 * server. Body is the browser's offer SDP (Content-Type text/plain or
 * application/sdp); the response is a JSON object
 * {"sdp":"<answer>","candidates":[...]} the browser applies with
 * setRemoteDescription + addIceCandidate. One round trip, no external server.
 */
static esp_err_t webrtc_offer_post(httpd_req_t *req)
{
    AUTH_GATE(req);
    int total = req->content_len;
    if (total <= 0 || total > 8192) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "offer size");
    }
    char *offer = malloc((size_t)total + 1);
    const size_t resp_cap = 8192;
    char *resp = malloc(resp_cap);
    if (!offer || !resp) {
        free(offer);
        free(resp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, offer + got, total - got);
        if (r <= 0) {
            free(offer);
            free(resp);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
        }
        got += r;
    }
    offer[total] = '\0';

    size_t resp_len = 0;
    esp_err_t er = webrtc_kvm_handle_offer(offer, (size_t)total, resp, resp_cap, &resp_len);
    free(offer);
    if (er != ESP_OK) {
        free(resp);
        return httpd_resp_send_custom_err(req, "503 Service Unavailable", esp_err_to_name(er));
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t sent = httpd_resp_send(req, resp, resp_len);
    free(resp);
    return sent;
}

/**
 * GET /webrtc/ice - the ICE-server list the browser should build its
 * RTCPeerConnection with: the public STUN server plus, when a TURN relay is
 * configured, a `turn:` entry with freshly derived short-lived credentials
 * (same coturn the device relays through). The browser must fetch this before
 * creating the offer so its candidate gathering includes relay candidates.
 * Auth-gated: it hands out usable TURN credentials.
 */
static esp_err_t webrtc_ice_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char resp[512];
    size_t resp_len = 0;
    if (webrtc_kvm_ice_config_json(resp, sizeof(resp), &resp_len) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ice");
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, resp_len);
}
#endif

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

/** GET /stream-fps optional query `fps=0..60` sets the max-FPS cap (0 = uncapped);
 *  response body is the current cap (text/plain). Each viewer still adapts below
 *  the cap when its link is congested. */
static esp_err_t stream_fps_get(httpd_req_t *req)
{
    AUTH_GATE(req);
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "fps", val, sizeof(val)) == ESP_OK) {
            int f = atoi(val);
            if (f >= 0 && f <= 60) {
                g_jpeg_frame.stream_max_fps = (uint8_t)f;
                (void)stream_max_fps_save_to_nvs((uint8_t)f);
            }
        }
    }
    char resp[16];
    int n = snprintf(resp, sizeof(resp), "%u\n", (unsigned)g_jpeg_frame.stream_max_fps);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stream-fps");
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
    int my_fd = httpd_req_to_sockfd(req);

    sock_set_nodelay(req);
    jpeg_frame_stream_enter();
    /* Send the current frame immediately on connect: with change detection the
     * publisher goes silent on a static screen, so a new viewer must get the
     * latest published frame now instead of a blank canvas until something moves.
     * (last_seq = seq-1 makes the first loop iteration send the current front.) */
    uint32_t last_seq = g_jpeg_frame.frame_seq - 1u;

    /* Rate control (per viewer): last_send_us gates against the max-FPS cap;
     * eff_extra_us is the adaptive back-off added on top when this link is
     * congested (0 = run at the cap). See the send path below. */
    int64_t last_send_us = 0;
    uint32_t eff_extra_us = 0;

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

        /* Honor the max-FPS cap plus this link's adaptive back-off: if the newest
         * frame arrived sooner than the target interval, drop the intervening
         * frames (skip to the newest published) rather than sending them. */
        {
            uint8_t cap = g_jpeg_frame.stream_max_fps;
            uint32_t cap_us = cap ? (1000000u / cap) : 0;
            uint32_t target_us = cap_us > eff_extra_us ? cap_us : eff_extra_us;
            if (target_us && last_send_us != 0) {
                if ((esp_timer_get_time() - last_send_us) < (int64_t)target_us) {
                    last_seq = g_jpeg_frame.frame_seq; /* wait for a fresher frame */
                    continue;
                }
            }
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
        int64_t send_t0 = esp_timer_get_time();
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
        /* Adapt to link speed: a send that eats most of the frame budget means
         * the socket is back-pressured (slow client / thin tunnel), so raise this
         * viewer's interval toward a 1 fps floor; a fast send eases it back to the
         * cap. This keeps a congested stream alive at a lower rate instead of
         * blocking until send_wait_timeout and forcing a browser reconnect. */
        {
            int64_t send_us = esp_timer_get_time() - send_t0;
            uint8_t cap = g_jpeg_frame.stream_max_fps;
            uint32_t budget = cap ? (1000000u / cap) : (1000000u / 60u);
            if (send_us > (int64_t)(budget - budget / 4)) {
                uint32_t base = eff_extra_us ? eff_extra_us : budget;
                eff_extra_us = base + base / 3;
                if (eff_extra_us > 1000000u) {
                    eff_extra_us = 1000000u; /* 1 fps floor */
                }
            } else if (send_us < (int64_t)(budget / 4)) {
                eff_extra_us = eff_extra_us > budget ? (eff_extra_us - eff_extra_us / 8) : 0;
                if (eff_extra_us < budget) {
                    eff_extra_us = 0;
                }
            }
        }
        last_send_us = esp_timer_get_time();
        last_seq = seq_snap;
    }
    jpeg_frame_stream_leave();
    httpd_resp_sendstr_chunk(req, NULL);
    if (httpd_req_async_handler_complete(req) != ESP_OK) {
        ESP_LOGW(TAG, "stream async complete failed");
    }
    stream_drop_session(my_fd);
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

    /* Newest viewer wins: close any previous stream so its worker exits and frees
     * the socket, rather than rejecting this one (a stuck worker would otherwise
     * lock everyone out and the browser would retry-storm the socket pool empty). */
    stream_take_session(async_req);

    BaseType_t created =
        xTaskCreate(stream_worker_task, "p4kvm_stream", STREAM_WORKER_STACK, async_req, STREAM_WORKER_PRIO, NULL);
    if (created != pdPASS) {
        stream_drop_session(httpd_req_to_sockfd(async_req));
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
    /* Bound a send to a stalled/half-open peer so a stream worker can't sit
     * blocked (holding its socket + the single-viewer slot) for the old 30 s. */
    cfg.send_wait_timeout = 12;
    /* Reap half-open sockets fast. The onboard C6 (esp_hosted over SDIO) blips
     * the WiFi link, leaving TCP connections half-open; without keepalive probes
     * they hold an lwIP socket until the peer happens to send, and a browser that
     * reconnects across the blip stacks dead sockets until the 24-socket pool is
     * exhausted (accept spins on ENFILE). Probe an idle peer after 5 s, then
     * every 3 s x3 -> dead connection dropped in ~14 s. */
    cfg.keep_alive_enable = true;
    cfg.keep_alive_idle = 5;
    cfg.keep_alive_interval = 3;
    cfg.keep_alive_count = 3;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.close_fn = http_sess_close_cb;
    /* Reclaim the least-recently-used session when full instead of wedging.
     * A browser (plus C6 WiFi blips) accumulates idle keep-alive and half-open
     * connections; with this off, httpd held every one until the peer cleanly
     * closed, filled all 12 slots + the lwIP pool, and then spun on accept()
     * ENFILE forever (HTTP dead while ICMP stayed up). Safe now that the stream
     * uses newest-viewer-wins: if the active /stream is ever the LRU victim, the
     * browser's next reconnect simply re-establishes it. */
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 12;
    /* Must exceed the number of httpd_register_uri_handler calls below, or the
     * last handlers silently fail to register ("no slots left") and 404. That
     * dropped /ws and /serial once MSC + serial were added, and the browser's
     * WS reconnect storm against the missing /ws exhausted the socket pool
     * (accept ENFILE) - the long-hunted "HTTP wedge". Count: root, favicon,
     * stream, jpeg-quality, stream-fps, stats, atx, video-mode, config x2,
     * media x3, ws, audio, serial, webrtc/offer, webrtc/ice = 18. Keep headroom. */
    cfg.max_uri_handlers = 21;

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
    httpd_uri_t u_stream_fps = {.uri = "/stream-fps", .method = HTTP_GET, .handler = stream_fps_get};
    httpd_register_uri_handler(h, &u_stream_fps);
    httpd_uri_t u_stats = {.uri = "/stats", .method = HTTP_GET, .handler = stats_get};
    httpd_register_uri_handler(h, &u_stats);
    httpd_uri_t u_atx = {.uri = "/atx", .method = HTTP_POST, .handler = atx_post};
    httpd_register_uri_handler(h, &u_atx);
    httpd_uri_t u_vmode = {.uri = "/video-mode", .method = HTTP_POST, .handler = video_mode_post};
    httpd_register_uri_handler(h, &u_vmode);
    httpd_uri_t u_cfg_get = {.uri = "/config", .method = HTTP_GET, .handler = config_get};
    httpd_register_uri_handler(h, &u_cfg_get);
    httpd_uri_t u_cfg_post = {.uri = "/config", .method = HTTP_POST, .handler = config_post};
    httpd_register_uri_handler(h, &u_cfg_post);
    httpd_uri_t u_media_status = {.uri = "/media/status", .method = HTTP_GET, .handler = media_status_get};
    httpd_register_uri_handler(h, &u_media_status);
    httpd_uri_t u_media_image = {.uri = "/media/image", .method = HTTP_POST, .handler = media_image_post};
    httpd_register_uri_handler(h, &u_media_image);
    httpd_uri_t u_media_eject = {.uri = "/media/eject", .method = HTTP_POST, .handler = media_eject_post};
    httpd_register_uri_handler(h, &u_media_eject);
#if CONFIG_P4KVM_WEBRTC_ENABLE
    httpd_uri_t u_webrtc_offer = {.uri = "/webrtc/offer", .method = HTTP_POST, .handler = webrtc_offer_post};
    httpd_register_uri_handler(h, &u_webrtc_offer);
    httpd_uri_t u_webrtc_ice = {.uri = "/webrtc/ice", .method = HTTP_GET, .handler = webrtc_ice_get};
    httpd_register_uri_handler(h, &u_webrtc_ice);
#endif
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
    if (audio_stream_available()) {
        httpd_uri_t u_audio = {
            .uri = "/audio",
            .method = HTTP_GET,
            .handler = audio_ws_handler,
            .is_websocket = true,
#if CONFIG_P4KVM_AUTH_ENABLE
            .ws_pre_handshake_cb = ws_auth_pre_handshake,
#endif
        };
        httpd_register_uri_handler(h, &u_audio);
    }
    if (usb_serial_available()) {
        httpd_uri_t u_serial = {
            .uri = "/serial",
            .method = HTTP_GET,
            .handler = serial_ws_handler,
            .is_websocket = true,
#if CONFIG_P4KVM_AUTH_ENABLE
            .ws_pre_handshake_cb = ws_auth_pre_handshake,
#endif
        };
        httpd_register_uri_handler(h, &u_serial);
    }
    return h;
}
