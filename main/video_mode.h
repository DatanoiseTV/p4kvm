/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Capture video mode, chosen at boot from NVS (default 720p60). A mode
 * change requires re-negotiating with the HDMI source (new EDID + HPD
 * cycle) and re-sizing every DMA buffer, so switching restarts the device.
 */
typedef enum {
    VIDEO_MODE_720P60 = 0, /**< 1280x720, EDID offers 60 Hz - default: one third the MJPEG bitrate of 1080p */
    VIDEO_MODE_1080P30 = 1,
} video_mode_t;

/** Load the persisted mode (call once early, after NVS init). */
void video_mode_init(void);

video_mode_t video_mode_get(void);
const char *video_mode_name(void);
uint32_t video_mode_hres(void);
uint32_t video_mode_vres(void);

/** EDID blob advertised to the HDMI source for the active mode. */
const uint8_t *video_mode_edid(size_t *len);

/**
 * Persist a new mode by name ("720p60" / "1080p30") and restart the device
 * shortly after (delay lets the HTTP response flush). ESP_ERR_INVALID_ARG on
 * unknown name; no restart if the mode is already active.
 */
esp_err_t video_mode_set_and_reboot(const char *name);

/** Restart the device after a short delay (lets an HTTP response flush). */
void video_mode_schedule_restart(void);

#ifdef __cplusplus
}
#endif
