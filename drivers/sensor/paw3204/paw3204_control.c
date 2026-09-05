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
#define MIN_AUTOMOUSE_TIMEOUT_MS     200
#define MAX_AUTOMOUSE_TIMEOUT_MS     3000
#define AUTOMOUSE_TIMEOUT_STEP_MS    100
#define SNIPER_SPEED_DIV 6           // 3x slower than normal (div 6)
#define DEFAULT_ROTATION_ANGLE 0     // 0 deg default (no tilt)
#define MIN_ROTATION_ANGLE -180
#define MAX_ROTATION_ANGLE 180
#define ROTATION_STEP_DEG 10
#define ROTATION_SCALE 1024

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

struct rot_trig {
    int16_t cos_val;
    int16_t sin_val;
};

// 36-Step Fixed-Point (x1024) Trigonometric Table for 0 to 350 deg in 10-deg steps
static const struct rot_trig s_rot_table[36] = {
    /*   0 deg */ {  1024,     0 },
    /*  10 deg */ {  1008,   178 },
    /*  20 deg */ {   962,   350 },
    /*  30 deg */ {   887,   512 },
    /*  40 deg */ {   784,   658 },
    /*  50 deg */ {   658,   784 },
    /*  60 deg */ {   512,   887 },
    /*  70 deg */ {   350,   962 },
    /*  80 deg */ {   178,  1008 },
    /*  90 deg */ {     0,  1024 },
    /* 100 deg */ {  -178,  1008 },
    /* 110 deg */ {  -350,   962 },
    /* 120 deg */ {  -512,   887 },
    /* 130 deg */ {  -658,   784 },
    /* 140 deg */ {  -784,   658 },
    /* 150 deg */ {  -887,   512 },
    /* 160 deg */ {  -962,   350 },
    /* 170 deg */ { -1008,   178 },
    /* 180 deg */ { -1024,     0 },
    /* 190 deg */ { -1008,  -178 },
    /* 200 deg */ {  -962,  -350 },
    /* 210 deg */ {  -887,  -512 },
    /* 220 deg */ {  -784,  -658 },
    /* 230 deg */ {  -658,  -784 },
    /* 240 deg */ {  -512,  -887 },
    /* 250 deg */ {  -350,  -962 },
    /* 260 deg */ {  -178, -1008 },
    /* 270 deg */ {     0, -1024 },
    /* 280 deg */ {   178, -1008 },
    /* 290 deg */ {   350,  -962 },
    /* 300 deg */ {   512,  -887 },
    /* 310 deg */ {   658,  -784 },
    /* 320 deg */ {   784,  -658 },
    /* 330 deg */ {   887,  -512 },
    /* 340 deg */ {   962,  -350 },
    /* 350 deg */ {  1008,  -178 },
};

struct tb_control_state {
    uint8_t speed_level;       // 1 (2.50x) to 16 (0.19x)
    uint8_t scroll_level;      // 1 (div 36) to 6 (div 6)
    int16_t rotation_angle;    // -180 to +180 deg (0 = default)
    uint16_t automouse_timeout_ms; // 200 to 3000 ms (800ms default)
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
    .rotation_angle = DEFAULT_ROTATION_ANGLE,
    .automouse_timeout_ms = DEFAULT_AUTOMOUSE_TIMEOUT_MS,
    .automouse_enabled = false,
    .automouse_active = false,
    .sniper_active = false,
    .scroll_mode = false,
    .scroll_axis_lock = true,
    .acceleration_enabled = true,
};

static int s_rot_x_accum = 0;
static int s_rot_y_accum = 0;

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

    if (settings_name_steq(name, "rot", &next) && !next) {
        if (len != sizeof(g_tb.rotation_angle)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &g_tb.rotation_angle, sizeof(g_tb.rotation_angle));
        if (rc >= 0) {
            if (g_tb.rotation_angle < MIN_ROTATION_ANGLE || g_tb.rotation_angle > MAX_ROTATION_ANGLE || (g_tb.rotation_angle % ROTATION_STEP_DEG != 0)) {
                g_tb.rotation_angle = DEFAULT_ROTATION_ANGLE;
            }
            LOG_INF("Loaded tb/rot: %d deg", g_tb.rotation_angle);
            return 0;
        }
        return rc;
    }

    if (settings_name_steq(name, "am_time", &next) && !next) {
        if (len != sizeof(g_tb.automouse_timeout_ms)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &g_tb.automouse_timeout_ms, sizeof(g_tb.automouse_timeout_ms));
        if (rc >= 0) {
            if (g_tb.automouse_timeout_ms < MIN_AUTOMOUSE_TIMEOUT_MS || g_tb.automouse_timeout_ms > MAX_AUTOMOUSE_TIMEOUT_MS) {
                g_tb.automouse_timeout_ms = DEFAULT_AUTOMOUSE_TIMEOUT_MS;
            }
            LOG_INF("Loaded tb/am_time: %d ms", g_tb.automouse_timeout_ms);
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
    settings_save_one("tb/rot", &g_tb.rotation_angle, sizeof(g_tb.rotation_angle));
    settings_save_one("tb/am_time", &g_tb.automouse_timeout_ms, sizeof(g_tb.automouse_timeout_ms));
    LOG_INF("Trackball settings saved to NVS (speed=%d, scrl=%d (div %d), am_en=%d, accel=%d, rot=%d deg, am_time=%d ms)",
            g_tb.speed_level, g_tb.scroll_level, s_scroll_div_table[g_tb.scroll_level - 1], am_val, accel_val, g_tb.rotation_angle, g_tb.automouse_timeout_ms);
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

    // Reset timeout timer
    k_work_reschedule(&g_tb.automouse_timeout_work, K_MSEC(g_tb.automouse_timeout_ms));
}

/* --- Pointer Speed & Motion Calculation --- */
void paw3204_control_calculate_motion(int dx, int dy, int *out_dx, int *out_dy)
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

void paw3204_control_automouse_time_up(void)
{
    if (g_tb.automouse_timeout_ms < MAX_AUTOMOUSE_TIMEOUT_MS) {
        g_tb.automouse_timeout_ms += AUTOMOUSE_TIMEOUT_STEP_MS;
        LOG_INF("Auto-Mouse timeout increased -> %d ms", g_tb.automouse_timeout_ms);
        schedule_settings_save();
    }
}

void paw3204_control_automouse_time_down(void)
{
    if (g_tb.automouse_timeout_ms > MIN_AUTOMOUSE_TIMEOUT_MS) {
        g_tb.automouse_timeout_ms -= AUTOMOUSE_TIMEOUT_STEP_MS;
        LOG_INF("Auto-Mouse timeout decreased -> %d ms", g_tb.automouse_timeout_ms);
        schedule_settings_save();
    }
}

void paw3204_control_automouse_time_reset(void)
{
    g_tb.automouse_timeout_ms = DEFAULT_AUTOMOUSE_TIMEOUT_MS;
    LOG_INF("Auto-Mouse timeout RESET -> %d ms", g_tb.automouse_timeout_ms);
    schedule_settings_save();
}

uint16_t paw3204_control_get_automouse_time(void)
{
    return g_tb.automouse_timeout_ms;
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

/* --- Dynamic Rotation Angle Control --- */
void paw3204_control_rotate_cw(void)
{
    if (g_tb.rotation_angle < MAX_ROTATION_ANGLE) {
        g_tb.rotation_angle += ROTATION_STEP_DEG;
        s_rot_x_accum = 0;
        s_rot_y_accum = 0;
        LOG_INF("Trackball rotation angle -> %d deg (CW)", g_tb.rotation_angle);
        schedule_settings_save();
    }
}

void paw3204_control_rotate_ccw(void)
{
    if (g_tb.rotation_angle > MIN_ROTATION_ANGLE) {
        g_tb.rotation_angle -= ROTATION_STEP_DEG;
        s_rot_x_accum = 0;
        s_rot_y_accum = 0;
        LOG_INF("Trackball rotation angle -> %d deg (CCW)", g_tb.rotation_angle);
        schedule_settings_save();
    }
}

void paw3204_control_rotate_reset(void)
{
    g_tb.rotation_angle = DEFAULT_ROTATION_ANGLE;
    s_rot_x_accum = 0;
    s_rot_y_accum = 0;
    LOG_INF("Trackball rotation angle RESET -> 0 deg");
    schedule_settings_save();
}

int16_t paw3204_control_get_rotation_angle(void)
{
    return g_tb.rotation_angle;
}

void paw3204_control_rotate_motion(int in_dx, int in_dy, int *out_dx, int *out_dy)
{
    int angle = g_tb.rotation_angle;
    if (angle == 0) {
        *out_dx = in_dx;
        *out_dy = in_dy;
        return;
    }

    int norm = angle % 360;
    if (norm < 0) {
        norm += 360;
    }
    int idx = (norm / 10) % 36;
    int c = s_rot_table[idx].cos_val;
    int s = s_rot_table[idx].sin_val;

    // Screen coordinate system (X: right +, Y: down +)
    // Clockwise rotation by angle theta:
    // x' = x * cos(theta) - y * sin(theta)
    // y' = x * sin(theta) + y * cos(theta)
    int rot_x = in_dx * c - in_dy * s;
    int rot_y = in_dx * s + in_dy * c;

    s_rot_x_accum += rot_x;
    s_rot_y_accum += rot_y;

    // Round to nearest integer with fractional accumulation to preserve micro-movements
    int res_dx = (s_rot_x_accum + (s_rot_x_accum >= 0 ? ROTATION_SCALE / 2 : -ROTATION_SCALE / 2)) / ROTATION_SCALE;
    int res_dy = (s_rot_y_accum + (s_rot_y_accum >= 0 ? ROTATION_SCALE / 2 : -ROTATION_SCALE / 2)) / ROTATION_SCALE;

    s_rot_x_accum -= res_dx * ROTATION_SCALE;
    s_rot_y_accum -= res_dy * ROTATION_SCALE;

    *out_dx = res_dx;
    *out_dy = res_dy;
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
