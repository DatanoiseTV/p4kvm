/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Mass Storage (MSC) LUN backed by a PSRAM ramdisk. The disk is exposed to
 * the host as a removable drive alongside the HID interface, so an image
 * (floppy/disk .img) can be presented to the target for booting or file
 * transfer. SD-card backing and web-UI image loading build on this.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

/* Allocate and format the ramdisk (empty FAT12 volume) in PSRAM. Idempotent.
 * Must be called before tinyusb_driver_install so the MSC LUN has a medium. */
esp_err_t usb_msc_init(void);

/* Whether an MSC LUN is available (ramdisk allocated). */
bool usb_msc_available(void);

/* Geometry for tud_msc_capacity_cb. */
uint32_t usb_msc_block_count(void);
uint16_t usb_msc_block_size(void);
uint32_t usb_msc_capacity_bytes(void);

/* Mount an arbitrary image into the PSRAM ramdisk:
 *   1. usb_msc_begin_image(size) - eject, (re)size the backing, zero it.
 *   2. usb_msc_load(offset, chunk, len) - stream the image bytes in.
 *   3. usb_msc_commit_image(writable) - re-insert; the host re-reads capacity.
 * begin returns ESP_ERR_INVALID_SIZE (0 or > cap) or ESP_ERR_NO_MEM. */
esp_err_t usb_msc_begin_image(uint32_t size_bytes);
void usb_msc_commit_image(bool writable);

/* Discard the mounted image and revert to the empty formatted floppy. */
esp_err_t usb_msc_eject_to_floppy(void);

/* Raw block access for the MSC read10/write10 callbacks. Bounds-checked;
 * return the number of bytes served, or -1 on an out-of-range request. */
int32_t usb_msc_read(uint32_t lba, uint32_t offset, void *buf, uint32_t len);
int32_t usb_msc_write(uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t len);

/* Medium-present / write-protect state (host sees insert/eject and RO). */
bool usb_msc_medium_present(void);
bool usb_msc_writable(void);
void usb_msc_set_writable(bool writable);

/* Overwrite ramdisk contents starting at byte offset (for loading an image).
 * Returns bytes written, or -1 if the range exceeds the disk. */
int32_t usb_msc_load(uint32_t byte_offset, const void *data, uint32_t len);
