/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Start TinyUSB composite HID (keyboard + relative mouse + absolute pointer) and report task. */
esp_err_t usb_hid_init(void);

/** USB HID connected and configured (host sees device). */
bool usb_hid_ready(void);

/**
 * Queue an absolute pointer ("virtual tablet") report.
 * @param abs_x,abs_y HID logical coordinates 0..32767 (resolution-independent; the web
 * client scales from its canvas). The host places the cursor exactly there - no drift.
 */
void usb_hid_mouse(uint8_t buttons, uint16_t abs_x, uint16_t abs_y, int8_t wheel);

/** Relative motion (mickeys). Prefer this for pointer-lock moves, usually one USB frame per update. */
void usb_hid_mouse_rel(uint8_t buttons, int16_t dx, int16_t dy, int8_t wheel);

/** Boot keyboard report: modifier bitmap + up to six non-zero key usages. */
void usb_hid_keyboard(uint8_t modifier, const uint8_t keycode[6]);

/**
 * Parse and dispatch one browser HID input message (the shared wire format used
 * by both the /ws WebSocket and the WebRTC HID data channel):
 *   buf[0]==0x01, len>=8: mouse - buttons, x/y or dx/dy (LE), wheel, relative flag
 *   buf[0]==0x02, len>=8: keyboard - modifier + 6 keycodes
 * Ignores anything shorter or unrecognized.
 */
void usb_hid_dispatch_report(const uint8_t *buf, size_t len);
