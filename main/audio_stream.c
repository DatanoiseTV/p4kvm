/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * HDMI audio capture, following the h2c-rpi approach: the TC358743 already
 * emits the HDMI source's audio on its I2S pads (configured in
 * tc358743.c/set_hdmi_audio; the bridge is I2S master with clocks derived
 * from the HDMI stream). Three jumper wires bring BCK/LRCK/DATA to P4 GPIOs;
 * the P4 receives as I2S slave and streams raw PCM S16LE stereo over the
 * /audio WebSocket to the browser.
 */
#include "audio_stream.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "p4kvm_audio";

#if CONFIG_P4KVM_AUDIO_ENABLE

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * HDMI sources ship 48 kHz almost universally (it is the only rate HDMI
 * requires sinks to support); the EDID advertises 48 kHz only. The rate is
 * used for driver buffer sizing - the actual bit clock comes from the
 * TC358743, so a 44.1 kHz source would still capture, just resample-labelled
 * wrong in the browser.
 */
#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHUNK_FRAMES 480 /* 10 ms per WebSocket frame */

static i2s_chan_handle_t s_rx;
static SemaphoreHandle_t s_client_mu;
static httpd_handle_t s_client_hd;
static int s_client_fd = -1;
static volatile bool s_streaming;

bool audio_stream_available(void)
{
    return CONFIG_P4KVM_AUDIO_I2S_BCK_GPIO >= 0 && CONFIG_P4KVM_AUDIO_I2S_WS_GPIO >= 0 &&
           CONFIG_P4KVM_AUDIO_I2S_DIN_GPIO >= 0;
}

const char *audio_stream_status_str(void)
{
    if (!audio_stream_available() || !s_rx) {
        return "off";
    }
    return s_streaming ? "streaming" : "idle";
}

void audio_stream_on_sock_close(int sockfd)
{
    if (!s_client_mu) {
        return;
    }
    if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(500)) != pdTRUE) {
        return;
    }
    if (sockfd == s_client_fd) {
        s_client_fd = -1;
        s_streaming = false;
    }
    xSemaphoreGive(s_client_mu);
}

esp_err_t audio_ws_handler(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        /* Listener never sends payload; ignore anything but the handshake. */
        return ESP_OK;
    }
    if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_OK;
    }
    int fd = httpd_req_to_sockfd(req);
    if (s_client_fd >= 0 && s_client_fd != fd) {
        httpd_sess_trigger_close(req->handle, s_client_fd);
    }
    s_client_hd = req->handle;
    s_client_fd = fd;
    xSemaphoreGive(s_client_mu);
    ESP_LOGI(TAG, "audio listener connected (fd %d)", fd);
    return ESP_OK;
}

static void audio_task(void *arg)
{
    (void)arg;
    /* 32-bit slots: the TC358743 fills up to 24 valid bits per slot; the
     * significant bits sit at the top, so >>16 yields S16. */
    static int32_t raw[AUDIO_CHUNK_FRAMES * 2];
    static int16_t pcm[AUDIO_CHUNK_FRAMES * 2];

    while (1) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, raw, sizeof(raw), &got, 1000);
        if (err != ESP_OK || got == 0) {
            /* No bit clock (source muted/asleep or wires absent): idle calmly. */
            s_streaming = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        size_t samples = got / sizeof(int32_t);
        for (size_t i = 0; i < samples; i++) {
            pcm[i] = (int16_t)(raw[i] >> 16);
        }

        httpd_handle_t hd = NULL;
        int fd = -1;
        if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
            hd = s_client_hd;
            fd = s_client_fd;
            xSemaphoreGive(s_client_mu);
        }
        if (fd < 0) {
            s_streaming = false;
            continue;
        }
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = (uint8_t *)pcm,
            .len = samples * sizeof(int16_t),
        };
        if (httpd_ws_send_frame_async(hd, fd, &frame) == ESP_OK) {
            s_streaming = true;
        } else {
            audio_stream_on_sock_close(fd);
        }
    }
}

esp_err_t audio_stream_init(void)
{
    if (!audio_stream_available()) {
        ESP_LOGW(TAG, "audio enabled but I2S GPIOs not configured - skipping");
        return ESP_OK;
    }
    s_client_mu = xSemaphoreCreateMutex();
    if (!s_client_mu) {
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_SLAVE);
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_P4KVM_AUDIO_I2S_BCK_GPIO,
                .ws = CONFIG_P4KVM_AUDIO_I2S_WS_GPIO,
                .dout = I2S_GPIO_UNUSED,
                .din = CONFIG_P4KVM_AUDIO_I2S_DIN_GPIO,
                .invert_flags = {0},
            },
    };
    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_rx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s init/enable: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(audio_task, "audio", 4096, NULL, tskIDLE_PRIORITY + 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "HDMI audio capture: I2S slave BCK=%d WS=%d DIN=%d, S16LE %d Hz stereo on /audio",
             CONFIG_P4KVM_AUDIO_I2S_BCK_GPIO, CONFIG_P4KVM_AUDIO_I2S_WS_GPIO, CONFIG_P4KVM_AUDIO_I2S_DIN_GPIO,
             AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

#else /* !CONFIG_P4KVM_AUDIO_ENABLE */

bool audio_stream_available(void)
{
    return false;
}

const char *audio_stream_status_str(void)
{
    return "off";
}

void audio_stream_on_sock_close(int sockfd)
{
    (void)sockfd;
}

esp_err_t audio_ws_handler(httpd_req_t *req)
{
    (void)req;
    return ESP_OK;
}

esp_err_t audio_stream_init(void)
{
    ESP_LOGD(TAG, "audio disabled");
    return ESP_OK;
}

#endif
