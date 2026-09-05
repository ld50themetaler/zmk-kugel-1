/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/activity.h>
#include <zmk/usb.h>
#include <zmk/keymap.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include "kugel_indicator.h"

LOG_MODULE_REGISTER(kugel_indicator, CONFIG_ZMK_LOG_LEVEL);

#define LED_NODE DT_ALIAS(led0)

#if !DT_NODE_EXISTS(LED_NODE)
#error "led0 alias is not defined in devicetree!"
#endif

static const struct gpio_dt_spec s_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

enum indicator_mode {
    MODE_IDLE,
    MODE_ADVERTISING,
    MODE_SHOW_BLE,
    MODE_SHOW_BATTERY,
    MODE_SHOW_ALL_BLE,
    MODE_SHOW_ALL_PAUSE,
    MODE_SHOW_ALL_BATTERY,
    MODE_AM_PREVIEW_OFF,
};

enum am_led_mode {
    AM_LED_OFF = 0,
    AM_LED_DIM = 1,
    AM_LED_BREATHE = 2,
    AM_LED_MODE_COUNT = 3,
};

#define MOUSE_LAYER_ID 4

static struct {
    enum indicator_mode mode;
    uint8_t step;
    uint8_t target_pulses;
    bool ble_connected;
    bool is_sleeping;
    bool is_long_pulse;
    struct k_work_delayable work;
    struct k_work_delayable glow_work;
} g_ind;

static uint8_t g_am_led_mode = AM_LED_DIM;
static bool g_mouse_layer_active = false;
static bool g_preview_active = false;
static uint16_t g_preview_cycles_left = 0;

static struct k_timer g_am_pwm_timer;
static bool s_pwm_is_on = false;
static uint8_t s_breathe_step = 0;
static uint32_t s_current_on_time = 0;

#define BREATHE_TOTAL_STEPS 120

/* Smooth breathing curve (120 steps, ~2.4s cycle)
 * Duty values in ms (out of 20ms period): 1ms (5% dim resting) to 6ms (30% brightness)
 */
static const uint8_t s_breathe_duty[BREATHE_TOTAL_STEPS] = {
    // Phase 1: Gentle rise from dim to peak (40 steps = 800ms)
    1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 2, 3, 3, 3, 3, 3, 3, 4, 4,
    4, 4, 4, 5, 5, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    // Phase 2: Gentle fall from peak to dim (40 steps = 800ms)
    6, 6, 6, 6, 6, 6, 5, 5, 5, 5,
    5, 5, 4, 4, 4, 4, 4, 3, 3, 3,
    3, 3, 3, 2, 2, 2, 2, 2, 2, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    // Phase 3: Gentle resting glow at dim (40 steps = 800ms) - never fully dark!
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};

static void led_set(bool on)
{
    if (s_led.port != NULL) {
        gpio_pin_set_dt(&s_led, on ? 1 : 0);
    }
}

static void trigger_advertising(void);
static void start_am_pwm(void);
static void stop_am_pwm(void);

static void return_to_idle_or_advertising(void)
{
    if (g_mouse_layer_active && g_am_led_mode != AM_LED_OFF) {
        start_am_pwm();
        return;
    }

    led_set(false);
    g_ind.mode = MODE_IDLE;
    if (!zmk_usb_is_powered() && !zmk_ble_active_profile_is_connected()) {
        trigger_advertising();
    }
}

static void trigger_advertising(void)
{
    if (g_ind.mode >= MODE_SHOW_BLE || zmk_usb_is_powered()) {
        return; // Don't interrupt on-demand indicator or flash while USB powered
    }
    LOG_DBG("Triggering advertising indicator (5s double-pulse)");
    g_ind.mode = MODE_ADVERTISING;
    g_ind.step = 0;
    k_work_reschedule(&g_ind.work, K_MSEC(100));
}

static void am_pwm_timer_handler(struct k_timer *timer)
{
    if (g_ind.is_sleeping || g_ind.mode >= MODE_SHOW_BLE) {
        led_set(false);
        s_pwm_is_on = false;
        return;
    }

    if (!g_mouse_layer_active && !g_preview_active) {
        led_set(false);
        s_pwm_is_on = false;
        return;
    }

    if (s_pwm_is_on) {
        // Currently ON -> Turn OFF for remainder of 20ms cycle
        led_set(false);
        s_pwm_is_on = false;
        uint32_t off_time = 20 - s_current_on_time;
        if (off_time == 0) {
            off_time = 1;
        }
        k_timer_start(&g_am_pwm_timer, K_MSEC(off_time), K_NO_WAIT);
    } else {
        // Currently OFF -> Start new cycle (determine on_time)
        uint8_t effective_mode = g_am_led_mode;

        if (g_preview_active) {
            if (g_preview_cycles_left > 0) {
                g_preview_cycles_left--;
            } else {
                g_preview_active = false;
                led_set(false);
                s_pwm_is_on = false;
                return_to_idle_or_advertising();
                return;
            }
        }

        if (effective_mode == AM_LED_DIM) {
            s_current_on_time = 2; // 10% duty (2ms ON / 18ms OFF)
        } else if (effective_mode == AM_LED_BREATHE) {
            s_current_on_time = s_breathe_duty[s_breathe_step];
            s_breathe_step = (s_breathe_step + 1) % BREATHE_TOTAL_STEPS;
        } else {
            s_current_on_time = 0;
        }

        if (s_current_on_time == 0) {
            led_set(false);
            s_pwm_is_on = false;
            k_timer_start(&g_am_pwm_timer, K_MSEC(20), K_NO_WAIT);
        } else {
            led_set(true);
            s_pwm_is_on = true;
            k_timer_start(&g_am_pwm_timer, K_MSEC(s_current_on_time), K_NO_WAIT);
        }
    }
}

static void start_am_pwm(void)
{
    if (g_ind.is_sleeping || g_ind.mode >= MODE_SHOW_BLE) {
        return;
    }

    if (g_am_led_mode == AM_LED_OFF && !g_preview_active) {
        led_set(false);
        return;
    }

    s_pwm_is_on = false;
    s_breathe_step = 0;
    k_timer_start(&g_am_pwm_timer, K_NO_WAIT, K_NO_WAIT);
}

static void stop_am_pwm(void)
{
    k_timer_stop(&g_am_pwm_timer);
    s_pwm_is_on = false;
    g_preview_active = false;
    if (g_ind.mode < MODE_SHOW_BLE) {
        led_set(false);
    }
}

void kugel_indicator_mouse_layer_changed(bool active)
{
    g_mouse_layer_active = active;
    LOG_INF("Mouse layer active: %d, am_led_mode: %d", active, g_am_led_mode);

    if (active) {
        start_am_pwm();
    } else {
        stop_am_pwm();
        return_to_idle_or_advertising();
    }
}

void kugel_indicator_cycle_am_led_mode(void)
{
    g_am_led_mode = (g_am_led_mode + 1) % AM_LED_MODE_COUNT;
    LOG_INF("Auto-Mouse LED Mode cycled -> %d", g_am_led_mode);

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_save_one("ind/am_led", &g_am_led_mode, sizeof(g_am_led_mode));
    LOG_INF("Saved ind/am_led to NVS: %d", g_am_led_mode);
#endif

    stop_am_pwm();

    if (g_am_led_mode == AM_LED_OFF) {
        // Short blink (50ms) to indicate OFF
        led_set(true);
        k_work_cancel_delayable(&g_ind.work);
        g_ind.mode = MODE_AM_PREVIEW_OFF;
        k_work_reschedule(&g_ind.work, K_MSEC(50));
    } else if (g_am_led_mode == AM_LED_DIM) {
        // Preview DIM for 600ms (30 cycles of 20ms)
        g_preview_active = true;
        g_preview_cycles_left = 30;
        start_am_pwm();
    } else if (g_am_led_mode == AM_LED_BREATHE) {
        // Preview 1 full breath cycle (~2.4s)
        g_preview_active = true;
        g_preview_cycles_left = BREATHE_TOTAL_STEPS;
        start_am_pwm();
    }
}

uint8_t kugel_indicator_get_am_led_mode(void)
{
    return g_am_led_mode;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int ind_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
    const char *next;
    int rc;

    if (settings_name_steq(name, "am_led", &next) && !next) {
        if (len != sizeof(g_am_led_mode)) {
            return -EINVAL;
        }
        rc = read_cb(cb_arg, &g_am_led_mode, sizeof(g_am_led_mode));
        if (rc >= 0) {
            if (g_am_led_mode >= AM_LED_MODE_COUNT) {
                g_am_led_mode = AM_LED_DIM;
            }
            LOG_INF("Loaded ind/am_led: %d", g_am_led_mode);
            return 0;
        }
        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ind, "ind", NULL, ind_settings_set, NULL, NULL);
#endif

static void start_ble_sequence(enum indicator_mode next_mode)
{
    uint8_t profile_idx = zmk_ble_active_profile_index();
    g_ind.ble_connected = zmk_ble_active_profile_is_connected();
    g_ind.target_pulses = profile_idx + 1; // 1 to 5 pulses
    g_ind.step = 0;
    g_ind.mode = next_mode;
    g_ind.is_long_pulse = false;

    LOG_INF("Starting BLE indicator: Profile %d (%d pulses), connected=%d",
            profile_idx, g_ind.target_pulses, g_ind.ble_connected);

    k_work_reschedule(&g_ind.work, K_MSEC(50));
}

static void start_battery_sequence(enum indicator_mode next_mode)
{
    uint8_t soc = zmk_battery_state_of_charge();
    if (soc >= 75) {
        g_ind.target_pulses = 4;
        g_ind.is_long_pulse = false;
    } else if (soc >= 50) {
        g_ind.target_pulses = 3;
        g_ind.is_long_pulse = false;
    } else if (soc >= 25) {
        g_ind.target_pulses = 2;
        g_ind.is_long_pulse = false;
    } else {
        g_ind.target_pulses = 1;
        g_ind.is_long_pulse = true; // 1 long pulse for low battery (<25%)
    }

    g_ind.step = 0;
    g_ind.mode = next_mode;

    LOG_INF("Starting Battery indicator: SOC=%d%%, pulses=%d, long=%d",
            soc, g_ind.target_pulses, g_ind.is_long_pulse);

    k_work_reschedule(&g_ind.work, K_MSEC(50));
}

void kugel_indicator_trigger(uint8_t mode)
{
    if (g_ind.is_sleeping) {
        return;
    }

    stop_am_pwm();
    k_work_cancel_delayable(&g_ind.work);
    k_work_cancel_delayable(&g_ind.glow_work);
    led_set(false);

    switch (mode) {
    case IND_BLE:
        start_ble_sequence(MODE_SHOW_BLE);
        break;
    case IND_BAT:
        start_battery_sequence(MODE_SHOW_BATTERY);
        break;
    case IND_ALL:
        start_ble_sequence(MODE_SHOW_ALL_BLE);
        break;
    default:
        LOG_WRN("Unknown indicator trigger mode: %d", mode);
        break;
    }
}

static void glow_off_work_handler(struct k_work *work)
{
    if (g_ind.is_sleeping) {
        led_set(false);
        return;
    }

    if (g_mouse_layer_active && g_am_led_mode != AM_LED_OFF) {
        start_am_pwm();
    } else if (g_ind.mode < MODE_SHOW_BLE) {
        led_set(false);
    }
}

void kugel_indicator_key_press(void)
{
    if (g_ind.is_sleeping) {
        return;
    }

    // Do not interrupt on-demand indicator sequences
    if (g_ind.mode >= MODE_SHOW_BLE) {
        return;
    }

    // Low Battery Key Glow: when SOC <= 20%, blink LED for 25ms on each key press
    uint8_t soc = zmk_battery_state_of_charge();
    if (soc <= 20) {
        k_timer_stop(&g_am_pwm_timer);
        led_set(true);
        k_work_reschedule(&g_ind.glow_work, K_MSEC(25));
    }
}

static void indicator_work_handler(struct k_work *work)
{
    if (g_ind.is_sleeping) {
        led_set(false);
        g_ind.mode = MODE_IDLE;
        return;
    }

    switch (g_ind.mode) {
    case MODE_AM_PREVIEW_OFF:
        led_set(false);
        return_to_idle_or_advertising();
        break;

    case MODE_SHOW_BLE:
    case MODE_SHOW_ALL_BLE: {
        uint8_t max_pulse_steps = g_ind.target_pulses * 2;
        if (g_ind.step < max_pulse_steps) {
            if (g_ind.step % 2 == 0) {
                led_set(true);
                g_ind.step++;
                k_work_reschedule(&g_ind.work, K_MSEC(120));
            } else {
                led_set(false);
                g_ind.step++;
                k_work_reschedule(&g_ind.work, K_MSEC(180));
            }
        } else if (g_ind.step == max_pulse_steps) {
            if (g_ind.ble_connected) {
                led_set(true);
                g_ind.step++;
                k_work_reschedule(&g_ind.work, K_MSEC(600));
            } else {
                led_set(false);
                if (g_ind.mode == MODE_SHOW_ALL_BLE) {
                    g_ind.mode = MODE_SHOW_ALL_PAUSE;
                    k_work_reschedule(&g_ind.work, K_MSEC(800));
                } else {
                    return_to_idle_or_advertising();
                }
            }
        } else {
            led_set(false);
            if (g_ind.mode == MODE_SHOW_ALL_BLE) {
                g_ind.mode = MODE_SHOW_ALL_PAUSE;
                k_work_reschedule(&g_ind.work, K_MSEC(800));
            } else {
                return_to_idle_or_advertising();
            }
        }
        break;
    }

    case MODE_SHOW_ALL_PAUSE:
        led_set(false);
        start_battery_sequence(MODE_SHOW_ALL_BATTERY);
        break;

    case MODE_SHOW_BATTERY:
    case MODE_SHOW_ALL_BATTERY: {
        uint8_t max_pulse_steps = g_ind.target_pulses * 2;
        if (g_ind.step < max_pulse_steps) {
            if (g_ind.step % 2 == 0) {
                led_set(true);
                g_ind.step++;
                uint32_t on_time = g_ind.is_long_pulse ? 500 : 150;
                k_work_reschedule(&g_ind.work, K_MSEC(on_time));
            } else {
                led_set(false);
                g_ind.step++;
                k_work_reschedule(&g_ind.work, K_MSEC(200));
            }
        } else {
            return_to_idle_or_advertising();
        }
        break;
    }

    case MODE_ADVERTISING:
        switch (g_ind.step) {
        case 0:
            led_set(true);
            g_ind.step = 1;
            k_work_reschedule(&g_ind.work, K_MSEC(50));
            break;
        case 1:
            led_set(false);
            g_ind.step = 2;
            k_work_reschedule(&g_ind.work, K_MSEC(100));
            break;
        case 2:
            led_set(true);
            g_ind.step = 3;
            k_work_reschedule(&g_ind.work, K_MSEC(50));
            break;
        case 3:
        default:
            led_set(false);
            g_ind.step = 0;
            k_work_reschedule(&g_ind.work, K_MSEC(4800));
            break;
        }
        break;

    case MODE_IDLE:
    default:
        return_to_idle_or_advertising();
        break;
    }
}

static int ble_profile_listener(const zmk_event_t *eh)
{
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("BLE profile changed: index=%d", ev->index);
    kugel_indicator_trigger(IND_BLE);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_ble_indicator, ble_profile_listener);
ZMK_SUBSCRIPTION(kugel_ble_indicator, zmk_ble_active_profile_changed);

static int usb_conn_state_listener(const zmk_event_t *eh)
{
    const struct zmk_usb_conn_state_changed *ev = as_zmk_usb_conn_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("USB conn state changed: conn=%d", ev->conn_state);

    if (g_ind.mode < MODE_SHOW_BLE) {
        return_to_idle_or_advertising();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_usb_indicator, usb_conn_state_listener);
ZMK_SUBSCRIPTION(kugel_usb_indicator, zmk_usb_conn_state_changed);

static int activity_state_listener(const zmk_event_t *eh)
{
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->state == ZMK_ACTIVITY_SLEEP) {
        LOG_INF("Entering sleep, turning off LED indicator");
        g_ind.is_sleeping = true;
        stop_am_pwm();
        k_work_cancel_delayable(&g_ind.work);
        k_work_cancel_delayable(&g_ind.glow_work);
        led_set(false);
        g_ind.mode = MODE_IDLE;
    } else if (ev->state == ZMK_ACTIVITY_ACTIVE && g_ind.is_sleeping) {
        LOG_INF("Waking up from sleep");
        g_ind.is_sleeping = false;
        return_to_idle_or_advertising();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_activity_indicator, activity_state_listener);
ZMK_SUBSCRIPTION(kugel_activity_indicator, zmk_activity_state_changed);

static int layer_state_listener(const zmk_event_t *eh)
{
    const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->layer == MOUSE_LAYER_ID) {
        kugel_indicator_mouse_layer_changed(ev->state);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_layer_indicator, layer_state_listener);
ZMK_SUBSCRIPTION(kugel_layer_indicator, zmk_layer_state_changed);

static int kugel_indicator_init(void)
{
    if (!gpio_is_ready_dt(&s_led)) {
        LOG_ERR("LED GPIO is not ready");
        return -ENODEV;
    }

    int ret = gpio_pin_configure_dt(&s_led, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure LED pin: %d", ret);
        return ret;
    }

    k_work_init_delayable(&g_ind.work, indicator_work_handler);
    k_work_init_delayable(&g_ind.glow_work, glow_off_work_handler);
    k_timer_init(&g_am_pwm_timer, am_pwm_timer_handler, NULL);

    LOG_INF("Kugel indicator initialized successfully on P0.08");

    // Initial sequence: show all status, then idle/off
    kugel_indicator_trigger(IND_ALL);

    return 0;
}

SYS_INIT(kugel_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
