/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usb_msc.h"

#include <string.h>

#include "class/msc/msc.h"
#include "class/msc/msc_device.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "usb_msc";

/* Block size is fixed at 512 (the only value real BIOS/UEFI boot code assumes).
 * The disk starts as an empty 1.44 MB FAT12 floppy and can be re-mounted at any
 * size up to the PSRAM cap via usb_msc_begin_image(); a mounted .img/.iso
 * carries its own filesystem so this module only hand-formats the empty case. */
#define MSC_BLOCK_SIZE 512u
#define FLOPPY_BLOCKS 2880u /* 2880 * 512 = 1.44 MB */

/* Upper bound on a PSRAM-backed image. PSRAM is shared with the 1080p capture
 * ping-pong + JPEG buffers, so cap the ramdisk well below total PSRAM; larger
 * media needs SD-card / network streaming (a later backing). */
#define MSC_MAX_BYTES (12u * 1024u * 1024u)

static uint8_t *s_disk;             /* current backing, s_capacity_bytes long */
static size_t s_capacity_bytes;     /* allocated size of s_disk */
static uint32_t s_block_count = FLOPPY_BLOCKS; /* reported LUN size in blocks */
static bool s_writable = true;
static bool s_present = true;
/* Deferred UNIT ATTENTION: after a remount we report "medium may have changed"
 * on the next command so the host re-reads capacity instead of trusting the
 * stale geometry it cached for the previous image. */
static bool s_media_changed;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

/* Write a standard, empty FAT12 1.44 MB floppy: boot sector (BPB), two FATs
 * seeded with the media byte, and a root directory holding just the volume
 * label. Deterministic byte layout - no FatFs dependency. */
static void format_fat12_floppy(void)
{
    memset(s_disk, 0, (size_t)FLOPPY_BLOCKS * MSC_BLOCK_SIZE);

    uint8_t *bs = s_disk; /* boot sector = LBA 0 */
    bs[0] = 0xEB;
    bs[1] = 0x3C;
    bs[2] = 0x90;                 /* jmp */
    memcpy(&bs[0x03], "MSDOS5.0", 8);
    put16(&bs[0x0B], MSC_BLOCK_SIZE); /* bytes/sector */
    bs[0x0D] = 1;                     /* sectors/cluster */
    put16(&bs[0x0E], 1);              /* reserved sectors */
    bs[0x10] = 2;                     /* number of FATs */
    put16(&bs[0x11], 224);            /* root dir entries */
    put16(&bs[0x13], FLOPPY_BLOCKS);  /* total sectors (16-bit) */
    bs[0x15] = 0xF0;                  /* media descriptor (removable) */
    put16(&bs[0x16], 9);              /* sectors per FAT */
    put16(&bs[0x18], 18);             /* sectors per track */
    put16(&bs[0x1A], 2);              /* heads */
    bs[0x24] = 0x00;                  /* drive number */
    bs[0x26] = 0x29;                  /* extended boot signature */
    bs[0x27] = 0x12;
    bs[0x28] = 0x34;
    bs[0x29] = 0x56;
    bs[0x2A] = 0x78;                       /* volume serial */
    memcpy(&bs[0x2B], "P4KVM      ", 11);  /* volume label */
    memcpy(&bs[0x36], "FAT12   ", 8);      /* fs type */
    bs[0x1FE] = 0x55;
    bs[0x1FF] = 0xAA;                      /* boot signature */

    /* FAT copies start at LBA 1 and LBA 10 (reserved=1, sectors/FAT=9). */
    for (uint32_t fat = 0; fat < 2; fat++) {
        uint8_t *f = s_disk + (size_t)(1 + fat * 9) * MSC_BLOCK_SIZE;
        f[0] = 0xF0;
        f[1] = 0xFF;
        f[2] = 0xFF; /* reserved cluster entries */
    }

    /* Root directory starts after both FATs: LBA 1 + 2*9 = 19. First entry is
     * the volume label so the host shows a named, formatted, empty disk. */
    uint8_t *root = s_disk + (size_t)19 * MSC_BLOCK_SIZE;
    memcpy(root, "P4KVM      ", 11);
    root[11] = 0x08; /* ATTR_VOLUME_ID */
}

/* Grow/shrink the PSRAM backing to hold at least `bytes`. Preserves the pointer
 * on same-or-smaller requests to avoid churn. Returns ESP_ERR_NO_MEM without
 * touching the existing medium if the allocation fails. */
static esp_err_t ensure_capacity(size_t bytes)
{
    if (s_disk && bytes <= s_capacity_bytes) {
        return ESP_OK;
    }
    uint8_t *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) {
        ESP_LOGE(TAG, "image alloc %u KiB PSRAM failed", (unsigned)(bytes / 1024));
        return ESP_ERR_NO_MEM;
    }
    if (s_disk) {
        heap_caps_free(s_disk);
    }
    s_disk = p;
    s_capacity_bytes = bytes;
    return ESP_OK;
}

esp_err_t usb_msc_init(void)
{
    if (s_disk) {
        return ESP_OK;
    }
    esp_err_t err = ensure_capacity((size_t)FLOPPY_BLOCKS * MSC_BLOCK_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    s_block_count = FLOPPY_BLOCKS;
    format_fat12_floppy();
    s_present = true;
    s_writable = true;
    ESP_LOGI(TAG, "ramdisk ready: %u blocks x %u = %u KiB (FAT12)", (unsigned)FLOPPY_BLOCKS, MSC_BLOCK_SIZE,
             (unsigned)((FLOPPY_BLOCKS * MSC_BLOCK_SIZE) / 1024));
    return ESP_OK;
}

/* Take the medium offline and (re)size the backing for an incoming image. The
 * host sees an eject; usb_msc_commit_image() re-inserts once bytes are loaded.
 * size_bytes is rounded up to a whole block. */
esp_err_t usb_msc_begin_image(uint32_t size_bytes)
{
    if (size_bytes == 0 || size_bytes > MSC_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t blocks = (size_bytes + MSC_BLOCK_SIZE - 1) / MSC_BLOCK_SIZE;
    size_t bytes = (size_t)blocks * MSC_BLOCK_SIZE;

    s_present = false; /* eject before we touch the buffer so no READ10 races the realloc */
    esp_err_t err = ensure_capacity(bytes);
    if (err != ESP_OK) {
        return err;
    }
    memset(s_disk, 0, bytes);
    s_block_count = blocks;
    ESP_LOGI(TAG, "staging image: %u blocks (%u KiB)", (unsigned)blocks, (unsigned)(bytes / 1024));
    return ESP_OK;
}

/* Re-insert the medium after an image has been streamed in and arm the
 * media-changed notification so the host re-reads the new capacity. */
void usb_msc_commit_image(bool writable)
{
    s_writable = writable;
    s_present = true;
    s_media_changed = true;
    ESP_LOGI(TAG, "image mounted: %u blocks, %s", (unsigned)s_block_count, writable ? "rw" : "ro");
}

/* Drop back to the empty formatted floppy. */
esp_err_t usb_msc_eject_to_floppy(void)
{
    s_present = false;
    esp_err_t err = ensure_capacity((size_t)FLOPPY_BLOCKS * MSC_BLOCK_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    s_block_count = FLOPPY_BLOCKS;
    format_fat12_floppy();
    s_writable = true;
    s_present = true;
    s_media_changed = true;
    ESP_LOGI(TAG, "reverted to empty floppy");
    return ESP_OK;
}

bool usb_msc_available(void)
{
    return s_disk != NULL;
}
uint32_t usb_msc_block_count(void)
{
    return s_block_count;
}
uint16_t usb_msc_block_size(void)
{
    return MSC_BLOCK_SIZE;
}
uint32_t usb_msc_capacity_bytes(void)
{
    return (uint32_t)s_block_count * MSC_BLOCK_SIZE;
}
bool usb_msc_medium_present(void)
{
    return s_disk != NULL && s_present;
}
bool usb_msc_writable(void)
{
    return s_writable;
}
void usb_msc_set_writable(bool writable)
{
    s_writable = writable;
}

int32_t usb_msc_read(uint32_t lba, uint32_t offset, void *buf, uint32_t len)
{
    if (!s_disk || lba >= s_block_count) {
        return -1;
    }
    uint32_t byte = lba * MSC_BLOCK_SIZE + offset;
    if ((uint64_t)byte + len > (uint64_t)s_block_count * MSC_BLOCK_SIZE) {
        return -1;
    }
    memcpy(buf, s_disk + byte, len);
    return (int32_t)len;
}

int32_t usb_msc_write(uint32_t lba, uint32_t offset, const uint8_t *buf, uint32_t len)
{
    if (!s_disk || !s_writable || lba >= s_block_count) {
        return -1;
    }
    uint32_t byte = lba * MSC_BLOCK_SIZE + offset;
    if ((uint64_t)byte + len > (uint64_t)s_block_count * MSC_BLOCK_SIZE) {
        return -1;
    }
    memcpy(s_disk + byte, buf, len);
    return (int32_t)len;
}

int32_t usb_msc_load(uint32_t byte_offset, const void *data, uint32_t len)
{
    if (!s_disk) {
        return -1;
    }
    if ((uint64_t)byte_offset + len > (uint64_t)s_block_count * MSC_BLOCK_SIZE) {
        return -1;
    }
    memcpy(s_disk + byte_offset, data, len);
    return (int32_t)len;
}

/* ---------------- TinyUSB MSC callbacks ---------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "p4kvm   ", 8);
    memcpy(product_id, "Virtual Media   ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!usb_msc_medium_present()) {
        /* Report "no medium" so the host retries instead of erroring. */
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    if (s_media_changed) {
        /* One-shot UNIT ATTENTION: "NOT READY TO READY CHANGE, MEDIUM MAY HAVE
         * CHANGED" (0x28/0x00). Fail this probe so the host re-reads capacity. */
        s_media_changed = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = usb_msc_block_count();
    *block_size = usb_msc_block_size();
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return usb_msc_writable();
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    return usb_msc_read(lba, offset, buffer, bufsize);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    return usb_msc_write(lba, offset, buffer, bufsize);
}

/* Start/Stop Unit carries the host's eject request (load_eject, start=0). */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        return 0;
    case SCSI_CMD_START_STOP_UNIT:
        /* bit1 = LOEJ (load/eject), bit0 = START. Eject => hide the medium. */
        if ((scsi_cmd[4] & 0x02) && !(scsi_cmd[4] & 0x01)) {
            s_present = false;
        } else if (scsi_cmd[4] & 0x01) {
            s_present = true;
        }
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
