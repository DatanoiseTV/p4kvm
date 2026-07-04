/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ATX_OP_POWER_TAP,  /**< Short press (~300 ms): ACPI power event (boot / soft shutdown). */
    ATX_OP_POWER_HOLD, /**< Long press (5 s): PSU hard cut ("4-second override"). */
    ATX_OP_RESET_TAP,  /**< Short press (~300 ms) on the reset header. */
} atx_op_t;

/**
 * Configure the front-panel header GPIOs (P4KVM_ATX_* Kconfig). No-op when both
 * GPIOs are -1; atx_ctrl_press then returns ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t atx_ctrl_init(void);

/** True when the corresponding GPIO is wired (Kconfig != -1). */
bool atx_ctrl_power_available(void);
bool atx_ctrl_reset_available(void);

/**
 * Execute a button press asynchronously (returns immediately; a one-shot timer
 * releases the pin). ESP_ERR_INVALID_STATE while a press is still in progress,
 * ESP_ERR_NOT_SUPPORTED when the needed GPIO is not wired.
 */
esp_err_t atx_ctrl_press(atx_op_t op);

#ifdef __cplusplus
}
#endif
