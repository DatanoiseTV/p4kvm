/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#include "atx_ctrl.h"

#include "runtime_cfg.h"

#include <stdatomic.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "atx";

#define ATX_POWER_TAP_MS 300
#define ATX_POWER_HOLD_MS 5000
#define ATX_RESET_TAP_MS 300

/*
 * "Pressed" drives the GPIO to the active level; released returns it to the
 * inactive level. With the recommended NPN/optocoupler stage the GPIO is
 * active-high (transistor shorts the header pins to ground while driven).
 * Kconfig bools are undefined (not 0) when off, hence the ifdef.
 */
#ifdef CONFIG_P4KVM_ATX_ACTIVE_HIGH
#define ATX_ACTIVE_DEFAULT 1
#else
#define ATX_ACTIVE_DEFAULT 0
#endif
/* Resolved once at init from NVS (web UI Setup) with the Kconfig fallback. */
static int s_active_level = ATX_ACTIVE_DEFAULT;
#define ATX_ACTIVE_LEVEL s_active_level

typedef struct {
    int gpio;
    esp_timer_handle_t release_timer;
    /* Per pin: the power and reset headers are independent buttons, so a 5 s
     * power hold must not block a reset press. */
    atomic_bool busy;
} atx_pin_t;

static atx_pin_t s_power = {.gpio = -1};
static atx_pin_t s_reset = {.gpio = -1};

static bool atx_pin_available(const atx_pin_t *pin)
{
    return pin->gpio >= 0 && pin->release_timer != NULL;
}

static void atx_release_cb(void *arg)
{
    atx_pin_t *pin = (atx_pin_t *)arg;
    gpio_set_level(pin->gpio, !ATX_ACTIVE_LEVEL);
    atomic_store(&pin->busy, false);
    ESP_LOGI(TAG, "released GPIO %d", pin->gpio);
}

static esp_err_t atx_pin_init(atx_pin_t *pin, const char *name)
{
    if (pin->gpio < 0) {
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin->gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config %s", name);
    ESP_RETURN_ON_ERROR(gpio_set_level(pin->gpio, !ATX_ACTIVE_LEVEL), TAG, "release %s", name);
    esp_timer_create_args_t targs = {
        .callback = atx_release_cb,
        .arg = pin,
        .dispatch_method = ESP_TIMER_TASK,
        .name = name,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &pin->release_timer), TAG, "timer %s", name);
    ESP_LOGI(TAG, "%s button on GPIO %d (active %s)", name, pin->gpio, ATX_ACTIVE_LEVEL ? "high" : "low");
    return ESP_OK;
}

esp_err_t atx_ctrl_init(void)
{
    s_power.gpio = (int)runtime_cfg_get_i32(RT_KEY_ATX_POWER, CONFIG_P4KVM_ATX_POWER_GPIO);
    s_reset.gpio = (int)runtime_cfg_get_i32(RT_KEY_ATX_RESET, CONFIG_P4KVM_ATX_RESET_GPIO);
    s_active_level = runtime_cfg_get_i32(RT_KEY_ATX_ACTIVE_HIGH, ATX_ACTIVE_DEFAULT) ? 1 : 0;
    ESP_RETURN_ON_ERROR(atx_pin_init(&s_power, "atx_power"), TAG, "power");
    ESP_RETURN_ON_ERROR(atx_pin_init(&s_reset, "atx_reset"), TAG, "reset");
    return ESP_OK;
}

bool atx_ctrl_power_available(void)
{
    return atx_pin_available(&s_power);
}

bool atx_ctrl_reset_available(void)
{
    return atx_pin_available(&s_reset);
}

esp_err_t atx_ctrl_press(atx_op_t op)
{
    atx_pin_t *pin;
    uint32_t hold_ms;
    switch (op) {
    case ATX_OP_POWER_TAP:
        pin = &s_power;
        hold_ms = ATX_POWER_TAP_MS;
        break;
    case ATX_OP_POWER_HOLD:
        pin = &s_power;
        hold_ms = ATX_POWER_HOLD_MS;
        break;
    case ATX_OP_RESET_TAP:
        pin = &s_reset;
        hold_ms = ATX_RESET_TAP_MS;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (!atx_pin_available(pin)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&pin->busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t er = gpio_set_level(pin->gpio, ATX_ACTIVE_LEVEL);
    if (er == ESP_OK) {
        er = esp_timer_start_once(pin->release_timer, (uint64_t)hold_ms * 1000ull);
    }
    if (er != ESP_OK) {
        gpio_set_level(pin->gpio, !ATX_ACTIVE_LEVEL);
        atomic_store(&pin->busy, false);
        return er;
    }
    ESP_LOGI(TAG, "press GPIO %d for %lu ms", pin->gpio, (unsigned long)hold_ms);
    return ESP_OK;
}
