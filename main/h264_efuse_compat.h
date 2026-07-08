/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compatibility shim for espressif/esp_h264 1.3.6 on ESP-IDF 6.0.0.
 *
 * esp_h264's port/inc/esp_h264_efuse.h assumes ESP-IDF >= 6.0 provides
 * esp_efuse_is_flash_encryption_enabled() (added in a 6.0 development snapshot),
 * but IDF 6.0.0 as released only ships esp_flash_encryption_enabled() in
 * bootloader_support. Without this the component fails to compile with
 * -Werror=implicit-function-declaration.
 *
 * This header is force-included (-include) into every esp_h264 translation unit
 * from main/CMakeLists.txt. It provides the expected name as a static inline
 * that forwards to IDF's real API, so no cross-component symbol resolution is
 * needed - esp_flash_encryption_enabled() is in bootloader_support, which is
 * always linked. The fix survives esp_h264 being re-downloaded by the component
 * manager because it lives entirely in this project.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Declared here (rather than via esp_flash_encrypt.h, which is not on esp_h264's
 * include path) so this header has no include dependencies. */
bool esp_flash_encryption_enabled(void);

static inline bool esp_efuse_is_flash_encryption_enabled(void)
{
    return esp_flash_encryption_enabled();
}

#ifdef __cplusplus
}
#endif
