/*
 * Status Indicator Behavior Driver for ZMK
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_indicator

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/indicator.h>
#include <zmk/behavior.h>
#include <zephyr/logging/log.h>

#include "kugel_indicator.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    switch (binding->param1) {
    case IND_BLE:
    case IND_BAT:
    case IND_ALL:
        kugel_indicator_trigger(binding->param1);
        break;
    case IND_AM_LED:
        kugel_indicator_cycle_am_led_mode();
        break;
    default:
        LOG_WRN("Unknown indicator command: %d", binding->param1);
        return -ENOTSUP;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_indicator_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

static int behavior_indicator_init(const struct device *dev)
{
    return 0;
}

#define BEHAVIOR_INDICATOR_DEVICE(n)                                                       \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_indicator_init, NULL, NULL, NULL, POST_KERNEL,     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                           \
                            &behavior_indicator_driver_api);

DT_INST_FOREACH_STATUS_OKAY(BEHAVIOR_INDICATOR_DEVICE)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
