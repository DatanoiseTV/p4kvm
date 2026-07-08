/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebRTC KVM session: one browser viewer at a time. The device is the answerer
 * (controlled role) - the browser creates the offer, POSTs it to /webrtc/offer,
 * and this module drives esp_peer to produce the answer + ICE candidates, which
 * the HTTP handler returns as JSON. Video is a send-only hardware-H.264 track
 * (fed from capture_h264); keyboard and mouse ride a reliable SCTP data channel
 * on the same peer connection (label "hid"), decoded by usb_hid_dispatch_report
 * - the identical wire format the /ws WebSocket uses.
 *
 * Signaling is automatic over the device's own HTTP server, so no external
 * signaling server is needed, and the browser falls back to MJPEG-over-/ws if
 * the peer connection cannot be established.
 */
#include "webrtc_kvm.h"

#include "sdkconfig.h"

#include "esp_log.h"

static const char *TAG = "p4kvm_rtc";

#if CONFIG_P4KVM_WEBRTC_ENABLE

#include <string.h>

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "capture_h264.h"
#include "usb_hid.h"
#include "video_mode.h"

#define RTC_HID_LABEL "hid"

/* Answer/candidate gathering window for a single HTTP round-trip. The browser
 * waits on the POST response, so we collect the answer SDP and whatever host
 * candidates ICE produces within this budget, then reply. On a LAN the reflexive
 * gathering is fast; there is no STUN by default. */
#define RTC_SDP_WAIT_MS 3000
#define RTC_CAND_SETTLE_MS 1800
#define RTC_MAX_CANDIDATES 24
#define RTC_MAX_SDP 4096
#define RTC_MAX_CAND_LEN 300

typedef enum {
    RTC_IDLE = 0,
    RTC_CONNECTING,
    RTC_CONNECTED,
} rtc_state_t;

/* Serializes handle_offer against itself (one negotiation at a time) and guards
 * the peer lifecycle vs the pc_task and the video sink. */
static SemaphoreHandle_t s_lock;

static const esp_peer_ops_t *s_ops;
static esp_peer_handle_t s_peer;
static TaskHandle_t s_pc_task;
static volatile bool s_pc_running;
static volatile rtc_state_t s_state;
static volatile bool s_video_ready; /* media path up: safe to send_video */

/* Signaling collection (written from esp_peer callbacks in the pc_task,
 * read by handle_offer in the HTTP task; guarded by s_sig_lock). */
static SemaphoreHandle_t s_sig_lock;
static char s_answer_sdp[RTC_MAX_SDP];
static volatile bool s_answer_ready;
static char s_cand[RTC_MAX_CANDIDATES][RTC_MAX_CAND_LEN];
static volatile int s_cand_count;

static void sig_reset(void)
{
    if (!s_sig_lock) {
        return;
    }
    xSemaphoreTake(s_sig_lock, portMAX_DELAY);
    s_answer_sdp[0] = '\0';
    s_answer_ready = false;
    s_cand_count = 0;
    xSemaphoreGive(s_sig_lock);
}

/* --- esp_peer callbacks (run in the pc_task main-loop context) --- */

static void rtc_video_sink(const uint8_t *nal, size_t len, bool keyframe, uint32_t pts_ms, void *ctx)
{
    (void)keyframe;
    (void)ctx;
    if (!s_peer || !s_video_ready || !s_ops || !s_ops->send_video) {
        return;
    }
    esp_peer_video_frame_t f = {
        .pts = pts_ms,
        .data = (uint8_t *)nal,
        .size = (int)len,
    };
    s_ops->send_video(s_peer, &f);
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    switch (state) {
    case ESP_PEER_STATE_CONNECTED:
        s_state = RTC_CONNECTED;
        break;
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
        /* Media + data path are up: start feeding H.264. Registering the sink
         * forces the encoder to emit an IDR first, so a static screen still
         * paints immediately instead of staying black. */
        s_video_ready = true;
        capture_h264_set_sink(rtc_video_sink, NULL);
        capture_h264_request_keyframe();
        ESP_LOGI(TAG, "data channel open; video streaming");
        break;
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_CLOSED:
    case ESP_PEER_STATE_CONNECT_FAILED:
        s_video_ready = false;
        capture_h264_set_sink(NULL, NULL);
        s_state = (state == ESP_PEER_STATE_CLOSED) ? RTC_IDLE : s_state;
        break;
    default:
        break;
    }
    return 0;
}

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;
    if (!msg || !msg->data || msg->size <= 0 || !s_sig_lock) {
        return 0;
    }
    xSemaphoreTake(s_sig_lock, portMAX_DELAY);
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        int n = msg->size < (int)sizeof(s_answer_sdp) - 1 ? msg->size : (int)sizeof(s_answer_sdp) - 1;
        memcpy(s_answer_sdp, msg->data, n);
        s_answer_sdp[n] = '\0';
        s_answer_ready = true;
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        if (s_cand_count < RTC_MAX_CANDIDATES) {
            const char *c = (const char *)msg->data;
            /* Strip a leading "a=" so the string is what RTCIceCandidate.candidate
             * expects (browsers reject the "a=" prefix). */
            if (msg->size >= 2 && c[0] == 'a' && c[1] == '=') {
                c += 2;
            }
            size_t n = strnlen(c, RTC_MAX_CAND_LEN - 1);
            memcpy(s_cand[s_cand_count], c, n);
            s_cand[s_cand_count][n] = '\0';
            s_cand_count++;
        }
    }
    xSemaphoreGive(s_sig_lock);
    return 0;
}

static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (!frame || !frame->data || frame->size <= 0) {
        return 0;
    }
    /* HID reports: same binary format as /ws. Other channels (if any) ignored. */
    usb_hid_dispatch_report(frame->data, (size_t)frame->size);
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "data channel '%s' (stream %u) open", ch && ch->label ? ch->label : "?",
             ch ? ch->stream_id : 0);
    return 0;
}

static int on_noop_video_info(esp_peer_video_stream_info_t *info, void *ctx)
{
    (void)info;
    (void)ctx;
    return 0;
}

static void pc_task(void *arg)
{
    (void)arg;
    while (s_pc_running) {
        if (s_peer && s_ops && s_ops->main_loop) {
            s_ops->main_loop(s_peer);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_pc_task = NULL;
    vTaskDelete(NULL);
}

/* Tear down the current peer + task. Caller holds s_lock. */
static void rtc_teardown(void)
{
    s_video_ready = false;
    capture_h264_set_sink(NULL, NULL);
    s_pc_running = false;
    /* Let pc_task exit its loop before closing the peer it dereferences. */
    for (int i = 0; i < 50 && s_pc_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_peer && s_ops && s_ops->close) {
        s_ops->close(s_peer);
    }
    s_peer = NULL;
    s_state = RTC_IDLE;
}

static esp_err_t build_answer_json(char *resp, size_t resp_cap, size_t *resp_len)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_sig_lock, portMAX_DELAY);
    cJSON_AddStringToObject(root, "sdp", s_answer_sdp);
    cJSON *arr = cJSON_AddArrayToObject(root, "candidates");
    for (int i = 0; i < s_cand_count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_cand[i]));
    }
    xSemaphoreGive(s_sig_lock);

    bool ok = cJSON_PrintPreallocated(root, resp, (int)resp_cap, false);
    cJSON_Delete(root);
    if (!ok) {
        return ESP_ERR_NO_MEM;
    }
    *resp_len = strlen(resp);
    return ESP_OK;
}

void webrtc_kvm_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_sig_lock) {
        s_sig_lock = xSemaphoreCreateMutex();
    }
    s_ops = esp_peer_get_default_impl();
    if (!s_ops) {
        ESP_LOGE(TAG, "esp_peer default impl unavailable");
    }
}

esp_err_t webrtc_kvm_handle_offer(const char *offer, size_t offer_len, char *resp, size_t resp_cap, size_t *resp_len)
{
    if (!offer || !offer_len || !resp || !resp_len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ops || !s_lock || !s_sig_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(4000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT; /* another negotiation in flight */
    }

    esp_err_t result = ESP_FAIL;
    do {
        /* Only one viewer: replace any existing session. */
        rtc_teardown();
        sig_reset();

        uint32_t w = video_mode_hres();
        uint32_t h = video_mode_vres();

        /* An ICE server is required: without one esp_peer's agent starts but
         * never gathers/binds a working candidate, so ICE never pairs and the
         * connection dies after a few seconds. A public STUN server also lets it
         * discover a server-reflexive candidate for off-LAN (tunnel) viewers.
         * static so it outlives this stack frame for the peer's lifetime. */
        static esp_peer_ice_server_cfg_t s_ice_servers[] = {
            {.stun_url = (char *)"stun:stun.l.google.com:19302"},
        };
        /* Browsers emit ~12 host candidates (one per interface, mDNS-obfuscated);
         * the default cap of 10 drops some ("Remote candidate over limited 10").
         * Raise it so the real reachable one is not the one dropped. */
        static esp_peer_default_cfg_t s_def_cfg = {
            .agent_recv_timeout = 500,
            .max_candidates = 24,
        };

        esp_peer_cfg_t cfg = {
            .role = ESP_PEER_ROLE_CONTROLLED,
            .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
            .server_lists = s_ice_servers,
            .server_num = sizeof(s_ice_servers) / sizeof(s_ice_servers[0]),
            .video_info = {.codec = ESP_PEER_VIDEO_CODEC_H264, .width = (int)w, .height = (int)h, .fps = 30},
            .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
            .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
            .enable_data_channel = true,
            .extra_cfg = &s_def_cfg,
            .extra_size = sizeof(s_def_cfg),
            .on_state = on_state,
            .on_msg = on_msg,
            .on_video_info = on_noop_video_info,
            .on_data = on_data,
            .on_channel_open = on_channel_open,
        };
        if (s_ops->open(&cfg, &s_peer) != ESP_PEER_ERR_NONE || !s_peer) {
            ESP_LOGE(TAG, "esp_peer open failed");
            result = ESP_FAIL;
            break;
        }
        s_state = RTC_CONNECTING;

        /* Pump the peer state machine. */
        s_pc_running = true;
        if (xTaskCreate(pc_task, "rtc_pc", 8192, NULL, 6, &s_pc_task) != pdPASS) {
            ESP_LOGE(TAG, "pc_task create failed");
            s_pc_running = false;
            rtc_teardown();
            result = ESP_ERR_NO_MEM;
            break;
        }

        if (s_ops->new_connection) {
            s_ops->new_connection(s_peer);
        }

        /* Feed the browser's offer; the answer arrives via on_msg. */
        esp_peer_msg_t om = {.type = ESP_PEER_MSG_TYPE_SDP, .data = (uint8_t *)offer, .size = (int)offer_len};
        if (s_ops->send_msg(s_peer, &om) != ESP_PEER_ERR_NONE) {
            ESP_LOGE(TAG, "send offer failed");
            rtc_teardown();
            result = ESP_FAIL;
            break;
        }

        /* Wait for the answer SDP, then let candidates settle briefly. */
        int64_t t0 = esp_timer_get_time();
        while (!s_answer_ready && (esp_timer_get_time() - t0) < (int64_t)RTC_SDP_WAIT_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!s_answer_ready) {
            ESP_LOGW(TAG, "no answer SDP within %d ms", RTC_SDP_WAIT_MS);
            rtc_teardown();
            result = ESP_ERR_TIMEOUT;
            break;
        }
        int64_t t1 = esp_timer_get_time();
        int last = -1;
        while ((esp_timer_get_time() - t1) < (int64_t)RTC_CAND_SETTLE_MS * 1000) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (s_cand_count == last) {
                /* No new candidate for a tick after we have at least one: done. */
                if (s_cand_count > 0) {
                    break;
                }
            }
            last = s_cand_count;
        }

        result = build_answer_json(resp, resp_cap, resp_len);
        if (result != ESP_OK) {
            rtc_teardown();
        }
    } while (0);

    xSemaphoreGive(s_lock);
    return result;
}

bool webrtc_kvm_connected(void)
{
    return s_state == RTC_CONNECTED;
}

const char *webrtc_kvm_state_str(void)
{
    switch (s_state) {
    case RTC_CONNECTING:
        return "connecting";
    case RTC_CONNECTED:
        return "connected";
    default:
        return "idle";
    }
}

#else /* !CONFIG_P4KVM_WEBRTC_ENABLE */

void webrtc_kvm_init(void) {}

esp_err_t webrtc_kvm_handle_offer(const char *offer, size_t offer_len, char *resp, size_t resp_cap, size_t *resp_len)
{
    (void)offer;
    (void)offer_len;
    (void)resp;
    (void)resp_cap;
    (void)resp_len;
    return ESP_ERR_NOT_SUPPORTED;
}

bool webrtc_kvm_connected(void) { return false; }
const char *webrtc_kvm_state_str(void) { return "off"; }

#endif
