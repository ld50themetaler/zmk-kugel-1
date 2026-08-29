/*
 * Trackball Behavior Driver for ZMK
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_trackball

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/trackball.h>
#include <zmk/behavior.h>
#include <zephyr/logging/log.h>

#include "paw3204_control.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event)
{
    switch (binding->param1) {
    case TB_SPD_UP:
        paw3204_control_speed_up();
        break;
    case TB_SPD_DN:
        paw3204_control_speed_down();
        break;
    case TB_AM_TOG:
        paw3204_control_toggle_automouse();
        break;
    case TB_SCRL_TOG:
        // Momentary scroll: activate scroll mode while key is held
        paw3204_control_set_scroll_mode(true);
        break;
    default:
        LOG_WRN("Unknown trackball command: %d", binding->param1);
        return -ENOTSUP;
    }

    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event)
{
    switch (binding->param1) {
    case TB_SCRL_TOG:
        // Momentary scroll: deactivate scroll mode when key is released
        paw3204_control_set_scroll_mode(false);
        break;
    default:
        break;
    }

    return 0;
}

static const struct behavior_driver_api behavior_trackball_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define TB_INST(n)                                                                               \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                              \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_trackball_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TB_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
