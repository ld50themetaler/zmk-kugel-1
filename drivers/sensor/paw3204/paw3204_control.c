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
#define DEFAULT_SPEED_LEVEL 8        // Level 8 = Normal (1.00x)
#define MAX_SPEED_LEVEL 16
#define DEFAULT_SCROLL_LEVEL 3       // Level 3 = Normal (div 20)
#define MAX_SCROLL_LEVEL 6
#define DEFAULT_AUTOMOUSE_ENABLE 1   // Enabled by default
#define DEFAULT_AUTOMOUSE_TIMEOUT_MS 800
#define SNIPER_SPEED_DIV 6           // 3x slower than normal (div 6)

struct speed_ratio {
    uint8_t num;
    uint8_t den;
};

// 16-Step Pointer Speed Table (Level 1: 2.50x to Level 16: 0.19x, Level 8: 1.00x default)
static const struct speed_ratio s_speed_table[MAX_SPEED_LEVEL] = {
    { 40, 16 }, // Level 1: 2.50x
    { 36, 16 }, // Level 2: 2.25x
    { 32, 16 }, // Level 3: 2.00x
    { 28, 16 }, // Level 4: 1.75x
    { 24, 16 }, // Level 5: 1.50x
    { 20, 16 }, // Level 6: 1.25x
    { 18, 16 }, // Level 7: 1.125x
    { 16, 16 }, // Level 8: 1.00x (Default / 1:1)
    { 14, 16 }, // Level 9: 0.875x
    { 12, 16 }, // Level 10: 0.75x
    { 10, 16 }, // Level 11: 0.625x
    {  8, 16 }, // Level 12: 0.50x
    {  6, 16 }, // Level 13: 0.375x
    {  5, 16 }, // Level 14: 0.3125x
    {  4, 16 }, // Level 15: 0.25x
    {  3, 16 }, // Level 16: 0.1875x
};

// Scroll Divisor Lookup Table (Level 1 to 6)
static const uint8_t s_scroll_div_table[MAX_SCROLL_LEVEL] = {
    36, // Level 1: Very Slow / Ultra Smooth
    28, // Level 2: Slow / Smooth
    20, // Level 3: Normal / Balanced (Default)
    15, // Level 4: Moderate / Responsive
    10, // Level 5: Fast
    6,  // Level 6: Ultra Fast (Legacy div 6)
};

struct tb_control_state {
    uint8_t speed_level;       // 1 (2.50x) to 16 (0.19x)
    uint8_t scroll_level;      // 1 (div 36) to 6 (div 6)
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
    .scroll_level = DEFAULT_SCROLL_LEVEL,
    .automouse_enabled = false,
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
            if (g_tb.speed_level < 1 || g_tb.speed_level > MAX_SPEED_LEVEL) {
                g_tb.speed_level = DEFAULT_SPEED_LEVEL;
            }
            LOG_INF("Loaded tb/speed: %d", g_tb.speed_level);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "scrl", &next) && !next) {
        if (len != sizeof(g_tb.scroll_level)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &g_tb.scroll_level, sizeof(g_tb.scroll_level));
        if (rc >= 0) {
            if (g_tb.scroll_level < 1 || g_tb.scroll_level > MAX_SCROLL_LEVEL) {
                g_tb.scroll_level = DEFAULT_SCROLL_LEVEL;
            }
            LOG_INF("Loaded tb/scrl: %d (div %d)", g_tb.scroll_level, s_scroll_div_table[g_tb.scroll_level - 1]);
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

    if (settings_name_steq(name, "accel", &next) && !next) {
        uint8_t val = 0;
        if (len != sizeof(val)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &val, sizeof(val));
        if (rc >= 0) {
            g_tb.acceleration_enabled = (val != 0);
            LOG_INF("Loaded tb/accel: %d", g_tb.acceleration_enabled);
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
    uint8_t accel_val = g_tb.acceleration_enabled ? 1 : 0;
    settings_save_one("tb/speed", &g_tb.speed_level, sizeof(g_tb.speed_level));
    settings_save_one("tb/scrl", &g_tb.scroll_level, sizeof(g_tb.scroll_level));
    settings_save_one("tb/am_en", &am_val, sizeof(am_val));
    settings_save_one("tb/accel", &accel_val, sizeof(accel_val));
    LOG_INF("Trackball settings saved to NVS (speed=%d, scrl=%d (div %d), am_en=%d, accel=%d)",
            g_tb.speed_level, g_tb.scroll_level, s_scroll_div_table[g_tb.scroll_level - 1], am_val, accel_val);
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

/* --- Pointer Speed & Motion Calculation --- */
void paw3204_control_calculate_motion(int8_t dx, int8_t dy, int *out_dx, int *out_dy)
{
    if (g_tb.sniper_active) {
        int f_dx = dx / SNIPER_SPEED_DIV;
        int f_dy = dy / SNIPER_SPEED_DIV;
        if (f_dx == 0 && dx != 0) f_dx = (dx > 0) ? 1 : -1;
        if (f_dy == 0 && dy != 0) f_dy = (dy > 0) ? 1 : -1;
        *out_dx = f_dx;
        *out_dy = f_dy;
        return;
    }

    uint8_t lvl = g_tb.speed_level;
    if (lvl < 1) lvl = 1;
    if (lvl > MAX_SPEED_LEVEL) lvl = MAX_SPEED_LEVEL;

    int num = s_speed_table[lvl - 1].num;
    int den = s_speed_table[lvl - 1].den;

    // Smooth Quadratic Acceleration Curve
    // Multiplier varies continuously without jarring thresholds:
    //   v <= 1: 0.60x (micro-precision)
    //   v == 2: 0.80x (micro-precision)
    //   v == 3: 1.00x (linear baseline)
    //   v >= 4: 1.00x + 0.03 * (v - 3)^2, capped at 3.50x (v >= 13)
    if (g_tb.acceleration_enabled) {
        int v = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        int factor = 1000; // base 1.0x in 1/1000th scale

        if (v <= 1) {
            factor = 600;  // 0.60x
        } else if (v == 2) {
            factor = 800;  // 0.80x
        } else {
            int diff = v - 3;
            factor = 1000 + 30 * diff * diff;
            if (factor > 3500) {
                factor = 3500; // Cap at 3.50x maximum boost
            }
        }

        num = num * factor;
        den = den * 1000;
    }

    int f_dx = (dx * num) / den;
    int f_dy = (dy * num) / den;

    if (f_dx == 0 && dx != 0) f_dx = (dx > 0) ? 1 : -1;
    if (f_dy == 0 && dy != 0) f_dy = (dy > 0) ? 1 : -1;

    *out_dx = f_dx;
    *out_dy = f_dy;
}

int paw3204_control_get_effective_div(int8_t dx, int8_t dy)
{
    return 1;
}

int paw3204_control_get_scroll_div(void)
{
    uint8_t lvl = g_tb.scroll_level;
    if (lvl < 1) lvl = 1;
    if (lvl > MAX_SCROLL_LEVEL) lvl = MAX_SCROLL_LEVEL;
    return s_scroll_div_table[lvl - 1];
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
        LOG_INF("Trackball speed increased -> Level %d", g_tb.speed_level);
        schedule_settings_save();
    }
}

void paw3204_control_speed_down(void)
{
    if (g_tb.speed_level < MAX_SPEED_LEVEL) {
        g_tb.speed_level++;
        LOG_INF("Trackball speed decreased -> Level %d", g_tb.speed_level);
        schedule_settings_save();
    }
}

uint8_t paw3204_control_get_speed_level(void)
{
    return g_tb.speed_level;
}

void paw3204_control_scroll_speed_up(void)
{
    if (g_tb.scroll_level < MAX_SCROLL_LEVEL) {
        g_tb.scroll_level++;
        LOG_INF("Scroll sensitivity increased -> Level %d (div %d)",
                g_tb.scroll_level, s_scroll_div_table[g_tb.scroll_level - 1]);
        schedule_settings_save();
    }
}

void paw3204_control_scroll_speed_down(void)
{
    if (g_tb.scroll_level > 1) {
        g_tb.scroll_level--;
        LOG_INF("Scroll sensitivity decreased -> Level %d (div %d)",
                g_tb.scroll_level, s_scroll_div_table[g_tb.scroll_level - 1]);
        schedule_settings_save();
    }
}

uint8_t paw3204_control_get_scroll_level(void)
{
    return g_tb.scroll_level;
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

void paw3204_control_toggle_acceleration(void)
{
    g_tb.acceleration_enabled = !g_tb.acceleration_enabled;
    LOG_INF("Trackball acceleration toggled: %d", g_tb.acceleration_enabled);
    schedule_settings_save();
}

bool paw3204_control_is_acceleration_enabled(void)
{
    return g_tb.acceleration_enabled;
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
