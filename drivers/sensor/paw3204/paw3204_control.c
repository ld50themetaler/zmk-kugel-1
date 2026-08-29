/*
 * Bit Trade One ADTB7M (PAW3204) Advanced Trackball Control Subsystem
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

#include "paw3204_control.h"

LOG_MODULE_REGISTER(paw3204_control, LOG_LEVEL_INF);

// Default Configuration Constants
#define DEFAULT_SPEED_LEVEL 2        // Level 2 = Normal (div 2)
#define DEFAULT_AUTOMOUSE_ENABLE 1   // Enabled by default
#define DEFAULT_AUTOMOUSE_TIMEOUT_MS 800
#define SNIPER_SPEED_DIV 6           // 3x slower than normal (div 6)

struct tb_control_state {
    uint8_t speed_level;       // 1 (fast, div 1) to 4 (slow, div 4)
    bool automouse_enabled;
    bool automouse_active;
    bool sniper_active;
    bool scroll_mode;
    bool scroll_axis_lock;
    bool acceleration_enabled;
    struct k_work_delayable automouse_timeout_work;
    struct k_work_delayable settings_save_work;
};

static struct tb_control_state g_tb = {
    .speed_level = DEFAULT_SPEED_LEVEL,
    .automouse_enabled = true,
    .automouse_active = false,
    .sniper_active = false,
    .scroll_mode = false,
    .scroll_axis_lock = true,
    .acceleration_enabled = true,
};

/* --- Settings (NVS) Persistence --- */
#if IS_ENABLED(CONFIG_SETTINGS)

static int tb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    const char *next;
    int rc;

    if (settings_name_steq(name, "speed", &next) && !next) {
        if (len != sizeof(g_tb.speed_level)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &g_tb.speed_level, sizeof(g_tb.speed_level));
        if (rc >= 0) {
            if (g_tb.speed_level < 1 || g_tb.speed_level > 4) {
                g_tb.speed_level = DEFAULT_SPEED_LEVEL;
            }
            LOG_INF("Loaded tb/speed: %d", g_tb.speed_level);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "am_en", &next) && !next) {
        uint8_t val = 0;
        if (len != sizeof(val)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &val, sizeof(val));
        if (rc >= 0) {
            g_tb.automouse_enabled = (val != 0);
            LOG_INF("Loaded tb/am_en: %d", g_tb.automouse_enabled);
            return 0;
        }
        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(tb, "tb", NULL, tb_settings_set, NULL, NULL);

static void settings_save_work_handler(struct k_work *work)
{
    uint8_t am_val = g_tb.automouse_enabled ? 1 : 0;
    settings_save_one("tb/speed", &g_tb.speed_level, sizeof(g_tb.speed_level));
    settings_save_one("tb/am_en", &am_val, sizeof(am_val));
    LOG_INF("Trackball settings saved to NVS (speed=%d, am_en=%d)", g_tb.speed_level, am_val);
}

static void schedule_settings_save(void)
{
    k_work_reschedule(&g_tb.settings_save_work, K_MSEC(1000));
}

#else
static void schedule_settings_save(void) {}
#endif /* IS_ENABLED(CONFIG_SETTINGS) */

/* --- Auto-Mouse Layer Logic --- */
static void automouse_timeout_handler(struct k_work *work)
{
    if (g_tb.automouse_active) {
        g_tb.automouse_active = false;
        if (zmk_keymap_layer_active(MOUSE_LAYER_ID)) {
            zmk_keymap_layer_deactivate(MOUSE_LAYER_ID, false);
            LOG_INF("Auto-Mouse: layer %d deactivated due to timeout", MOUSE_LAYER_ID);
        }
    }
}

void paw3204_control_on_motion(int8_t dx, int8_t dy)
{
    if (!g_tb.automouse_enabled) {
        return;
    }

    // When motion detected, activate MOUSE_LAYER if not active
    if (!g_tb.automouse_active) {
        g_tb.automouse_active = true;
        if (!zmk_keymap_layer_active(MOUSE_LAYER_ID)) {
            zmk_keymap_layer_activate(MOUSE_LAYER_ID, false);
            LOG_INF("Auto-Mouse: layer %d activated on motion", MOUSE_LAYER_ID);
        }
    }

    // Reset timeout timer (800ms)
    k_work_reschedule(&g_tb.automouse_timeout_work, K_MSEC(DEFAULT_AUTOMOUSE_TIMEOUT_MS));
}

/* --- Speed & Sniper Calculation --- */
int paw3204_control_get_effective_div(int8_t dx, int8_t dy)
{
    // 1. Sniper Mode takes top priority: slow, precise aiming
    if (g_tb.sniper_active) {
        return SNIPER_SPEED_DIV; // div 6
    }

    // 2. Base speed divisor based on speed_level (1: div 1, 2: div 2, 3: div 3, 4: div 4)
    int base_div = g_tb.speed_level;
    if (base_div < 1) base_div = 1;
    if (base_div > 4) base_div = 4;

    // 3. Mouse Acceleration curve:
    // If user moves gently (|dx|, |dy| <= 2): increase precision (divisor + 1)
    // If user flicks fast (|dx|, |dy| >= 8): decrease divisor (faster glide)
    if (g_tb.acceleration_enabled) {
        int max_delta = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        if (max_delta <= 2) {
            return base_div + 1; // Extra precision for tiny moves
        } else if (max_delta >= 8 && base_div > 1) {
            return base_div - 1; // Accelerated fast glide
        }
    }

    return base_div;
}

bool paw3204_control_is_sniper_active(void)
{
    return g_tb.sniper_active;
}

bool paw3204_control_is_scroll_mode(void)
{
    return g_tb.scroll_mode;
}

void paw3204_control_set_scroll_mode(bool enable)
{
    g_tb.scroll_mode = enable;
}

void paw3204_control_toggle_scroll_mode(void)
{
    g_tb.scroll_mode = !g_tb.scroll_mode;
    LOG_INF("Scroll mode toggled: %d", g_tb.scroll_mode);
}

void paw3204_control_speed_up(void)
{
    if (g_tb.speed_level > 1) {
        g_tb.speed_level--;
        LOG_INF("Trackball speed increased -> Level %d (div %d)", g_tb.speed_level, g_tb.speed_level);
        schedule_settings_save();
    }
}

void paw3204_control_speed_down(void)
{
    if (g_tb.speed_level < 4) {
        g_tb.speed_level++;
        LOG_INF("Trackball speed decreased -> Level %d (div %d)", g_tb.speed_level, g_tb.speed_level);
        schedule_settings_save();
    }
}

uint8_t paw3204_control_get_speed_level(void)
{
    return g_tb.speed_level;
}

void paw3204_control_toggle_automouse(void)
{
    g_tb.automouse_enabled = !g_tb.automouse_enabled;
    if (!g_tb.automouse_enabled && g_tb.automouse_active) {
        g_tb.automouse_active = false;
        k_work_cancel_delayable(&g_tb.automouse_timeout_work);
        if (zmk_keymap_layer_active(MOUSE_LAYER_ID)) {
            zmk_keymap_layer_deactivate(MOUSE_LAYER_ID, false);
        }
    }
    LOG_INF("Auto-Mouse toggled: %d", g_tb.automouse_enabled);
    schedule_settings_save();
}

bool paw3204_control_is_automouse_enabled(void)
{
    return g_tb.automouse_enabled;
}

/* --- Event Handlers (Layer & Keypress Listeners) --- */

static int layer_state_listener(const zmk_event_t *eh)
{
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->layer == SNIPE_LAYER_ID) {
        g_tb.sniper_active = ev->state;
        LOG_INF("Sniper mode %s", g_tb.sniper_active ? "ACTIVATED" : "DEACTIVATED");
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(tb_layer_listener, layer_state_listener);
ZMK_SUBSCRIPTION(tb_layer_listener, zmk_layer_state_changed);

static int position_state_listener(const zmk_event_t *eh)
{
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // If toggle drag-scroll mode is active, dismiss it on any click or key press
    if (g_tb.scroll_mode) {
        g_tb.scroll_mode = false;
        LOG_INF("Scroll mode dismissed on key/click position %d", ev->position);
    }

    // If auto-mouse is active and a non-mouse key is pressed:
    // Dismiss auto-mouse layer immediately so user can type seamlessly without delay
    if (g_tb.automouse_active) {
        // Excluded thumb key positions that belong to mouse actions (e.g. clicks / scrolls)
        // Matrix mapping: Row 0..2 are typing keys. Thumbs are index 36..42.
        if (ev->position < 36) {
            g_tb.automouse_active = false;
            k_work_cancel_delayable(&g_tb.automouse_timeout_work);
            if (zmk_keymap_layer_active(MOUSE_LAYER_ID)) {
                zmk_keymap_layer_deactivate(MOUSE_LAYER_ID, false);
                LOG_INF("Auto-Mouse: dismissed on typing key position %d", ev->position);
            }
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(tb_pos_listener, position_state_listener);
ZMK_SUBSCRIPTION(tb_pos_listener, zmk_position_state_changed);

/* --- Subsystem Initialization --- */
void paw3204_control_init(void)
{
    k_work_init_delayable(&g_tb.automouse_timeout_work, automouse_timeout_handler);
#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&g_tb.settings_save_work, settings_save_work_handler);
#endif
    LOG_INF("Kugel PAW3204 Advanced Control Subsystem initialized");
}
