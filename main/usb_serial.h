/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB CDC-ACM serial console. Presents a virtual serial port to the target
 * (a COM port on Windows, /dev/ttyACM* on Linux) as a third USB function
 * alongside HID and MSC, and bridges it to a browser terminal (xterm.js) over
 * the /serial WebSocket: bytes the target prints flow to the browser, and the
 * browser's keystrokes are written back to the target.
 *
 * Compiled only when CONFIG_TINYUSB_CDC_ENABLED is set; otherwise every entry
 * point is a no-op stub so the device enumerates as plain HID+MSC.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/* True when the CDC function is compiled in. */
bool usb_serial_available(void);

/* Bring up the CDC-ACM port and the RX pump task. Call AFTER the TinyUSB
 * driver is installed (the composite descriptor must already advertise CDC). */
esp_err_t usb_serial_init(void);

/* /serial WebSocket: GET registers the (single) terminal client; subsequent
 * frames are the browser's keystrokes, written straight to the target. */
esp_err_t serial_ws_handler(httpd_req_t *req);

/* Drop the terminal client if this socket was it (called from the httpd close hook). */
void usb_serial_on_sock_close(int sockfd);

/* "off" (not compiled), "closed" (host has not opened the port), or "open". */
const char *usb_serial_status_str(void);
