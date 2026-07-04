/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "capture.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "capture_priv.h"

static void camera_task(void *arg)
{
    (void)arg;
#if CONFIG_P4KVM_TEST_PATTERN
    capture_ctx_t *ctx = capture_testpat_init_start();
#else
    capture_ctx_t *ctx = capture_hw_init_start();
#endif
    if (!ctx) {
        return;
    }
    capture_mjpeg_run(ctx);
}

void capture_start(void)
{
    const uint32_t cam_stack = 10240;
    xTaskCreatePinnedToCore(camera_task, "cam", cam_stack, NULL, 5, NULL, 0);
}
