/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usb_serial.h"

#include "sdkconfig.h"

#if CONFIG_TINYUSB_CDC_ENABLED

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "usb_serial";

#define SERIAL_CDC_PORT TINYUSB_CDC_ACM_0
/* target -> browser staging. One HDMI-less console can burst; 8 KiB rides out a
 * browser stall without back-pressuring the CDC RX callback. */
#define SERIAL_RX_STREAM_BYTES 8192
/* Max WS payload drained per send; keeps frames small enough to interleave with
 * HID/MJPEG on the shared httpd sockets. */
#define SERIAL_TX_CHUNK 512

static httpd_handle_t s_client_hd;
static int s_client_fd = -1;
static SemaphoreHandle_t s_client_mu;
static StreamBufferHandle_t s_rx; /* bytes from target, awaiting the browser */
static volatile bool s_dtr;       /* host opened the port (DTR asserted) */

bool usb_serial_available(void)
{
    return true;
}

const char *usb_serial_status_str(void)
{
    return s_dtr ? "open" : "closed";
}

void usb_serial_on_sock_close(int sockfd)
{
    if (!s_client_mu) {
        return;
    }
    if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_client_fd == sockfd) {
            s_client_fd = -1;
            s_client_hd = NULL;
        }
        xSemaphoreGive(s_client_mu);
    }
}

/* CDC RX: the target wrote bytes. Drain the CDC FIFO into the stream buffer for
 * the pump task; never block here (runs in the TinyUSB task). */
static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    for (;;) {
        size_t n = 0;
        if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n) != ESP_OK || n == 0) {
            break;
        }
        if (s_rx) {
            /* Drop on overflow rather than stall the USB task: a wedged browser
             * must not back up the target's console. */
            xStreamBufferSend(s_rx, buf, n, 0);
        }
        if (n < sizeof(buf)) {
            break;
        }
    }
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    (void)itf;
    s_dtr = event->line_state_changed_data.dtr;
}

/* Pump: forward staged target bytes to the terminal client as WS frames. */
static void serial_pump_task(void *arg)
{
    (void)arg;
    static uint8_t buf[SERIAL_TX_CHUNK];
    for (;;) {
        size_t n = xStreamBufferReceive(s_rx, buf, sizeof(buf), portMAX_DELAY);
        if (n == 0) {
            continue;
        }
        httpd_handle_t hd = NULL;
        int fd = -1;
        if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(100)) == pdTRUE) {
            hd = s_client_hd;
            fd = s_client_fd;
            xSemaphoreGive(s_client_mu);
        }
        if (fd < 0) {
            continue; /* no terminal open: discard (console is fire-and-forget) */
        }
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_BINARY,
            .payload = buf,
            .len = n,
        };
        if (httpd_ws_send_frame_async(hd, fd, &frame) != ESP_OK) {
            usb_serial_on_sock_close(fd);
        }
    }
}

esp_err_t usb_serial_init(void)
{
    if (s_client_mu) {
        return ESP_OK;
    }
    s_client_mu = xSemaphoreCreateMutex();
    s_rx = xStreamBufferCreate(SERIAL_RX_STREAM_BYTES, 1);
    if (!s_client_mu || !s_rx) {
        return ESP_ERR_NO_MEM;
    }

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = SERIAL_CDC_PORT,
        .callback_rx = cdc_rx_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    ESP_RETURN_ON_ERROR(tinyusb_cdcacm_init(&acm_cfg), TAG, "cdcacm_init");

    BaseType_t ok = xTaskCreate(serial_pump_task, "serial_pump", 4096, NULL, tskIDLE_PRIORITY + 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CDC serial console ready on /serial");
    return ESP_OK;
}

/* Write browser keystrokes to the target. Bounded, non-blocking flush. */
static void serial_write_to_target(const uint8_t *data, size_t len)
{
    if (!s_dtr) {
        return; /* nothing on the other end has the port open */
    }
    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        size_t queued = tinyusb_cdcacm_write_queue(SERIAL_CDC_PORT, data + off, chunk);
        if (queued == 0) {
            break; /* TX FIFO full; drop the rest rather than block httpd */
        }
        off += queued;
        tinyusb_cdcacm_write_flush(SERIAL_CDC_PORT, 0);
    }
}

esp_err_t serial_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Auth already enforced by the pre-handshake callback. Single terminal
         * client, newest wins. */
        if (xSemaphoreTake(s_client_mu, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int fd = httpd_req_to_sockfd(req);
            if (s_client_fd >= 0 && s_client_fd != fd) {
                httpd_sess_trigger_close(req->handle, s_client_fd);
            }
            s_client_hd = req->handle;
            s_client_fd = fd;
            xSemaphoreGive(s_client_mu);
            ESP_LOGI(TAG, "terminal connected (fd %d)", fd);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt.type == HTTPD_WS_TYPE_CLOSE) {
        usb_serial_on_sock_close(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (pkt.len == 0 || pkt.len > 1024) {
        /* Terminal input frames are keystrokes/small pastes; ignore oversized. */
        return ESP_OK;
    }
    uint8_t buf[1024];
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    if (ret != ESP_OK) {
        return ret;
    }
    serial_write_to_target(buf, pkt.len);
    return ESP_OK;
}

#else /* !CONFIG_TINYUSB_CDC_ENABLED */

bool usb_serial_available(void)
{
    return false;
}
esp_err_t usb_serial_init(void)
{
    return ESP_OK;
}
esp_err_t serial_ws_handler(httpd_req_t *req)
{
    (void)req;
    return ESP_OK;
}
void usb_serial_on_sock_close(int sockfd)
{
    (void)sockfd;
}
const char *usb_serial_status_str(void)
{
    return "off";
}

#endif
