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
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/activity.h>
#include <zmk/usb.h>

#include "kugel_indicator.h"

LOG_MODULE_REGISTER(kugel_indicator, CONFIG_ZMK_LOG_LEVEL);

#define LED_NODE DT_ALIAS(led0)

#if !DT_NODE_EXISTS(LED_NODE)
#error "led0 alias is not defined in devicetree!"
#endif

static const struct gpio_dt_spec s_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

enum indicator_mode {
    MODE_IDLE,
    MODE_USB_POWERED,
    MODE_ADVERTISING,
    MODE_SHOW_BLE,
    MODE_SHOW_BATTERY,
    MODE_SHOW_ALL_BLE,
    MODE_SHOW_ALL_PAUSE,
    MODE_SHOW_ALL_BATTERY,
};

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

static void led_set(bool on)
{
    if (s_led.port != NULL) {
        gpio_pin_set_dt(&s_led, on ? 1 : 0);
    }
}

static void trigger_advertising(void);

static void return_to_idle_or_advertising(void)
{
    if (zmk_usb_is_powered()) {
        led_set(true);
        g_ind.mode = MODE_USB_POWERED;
        return;
    }

    led_set(false);
    g_ind.mode = MODE_IDLE;
    if (!zmk_ble_active_profile_is_connected()) {
        trigger_advertising();
    }
}

static void trigger_advertising(void)
{
    if (g_ind.mode >= MODE_SHOW_BLE || zmk_usb_is_powered()) {
        return; // Don't interrupt on-demand indicator or USB solid ON
    }
    LOG_DBG("Triggering advertising indicator (5s double-pulse)");
    g_ind.mode = MODE_ADVERTISING;
    g_ind.step = 0;
    k_work_reschedule(&g_ind.work, K_MSEC(100));
}

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

    if (zmk_usb_is_powered()) {
        led_set(true);
    } else if (g_ind.mode < MODE_SHOW_BLE) {
        led_set(false);
    }
}

void kugel_indicator_key_press(void)
{
    // If USB is powered, LED is already solid ON
    if (g_ind.is_sleeping || zmk_usb_is_powered()) {
        return;
    }

    // Do not interrupt on-demand indicator sequences
    if (g_ind.mode >= MODE_SHOW_BLE) {
        return;
    }

    // Low Battery Key Glow: when SOC <= 20%, blink LED for 25ms on each key press
    uint8_t soc = zmk_battery_state_of_charge();
    if (soc <= 20) {
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
    case MODE_SHOW_BLE:
    case MODE_SHOW_ALL_BLE: {
        // Step 0..2*N-1: Pulses for profile index
        // Each pulse: 120ms ON, 180ms OFF
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
            // Pulses complete. If connected, show 600ms solid ON!
            if (g_ind.ble_connected) {
                led_set(true);
                g_ind.step++;
                k_work_reschedule(&g_ind.work, K_MSEC(600));
            } else {
                // Not connected: finish BLE phase
                led_set(false);
                if (g_ind.mode == MODE_SHOW_ALL_BLE) {
                    g_ind.mode = MODE_SHOW_ALL_PAUSE;
                    k_work_reschedule(&g_ind.work, K_MSEC(800));
                } else {
                    return_to_idle_or_advertising();
                }
            }
        } else {
            // Connected solid ON finished
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
        // Pause between BLE and Battery displays (dark for 800ms)
        led_set(false);
        start_battery_sequence(MODE_SHOW_ALL_BATTERY);
        break;

    case MODE_SHOW_BATTERY:
    case MODE_SHOW_ALL_BATTERY: {
        // Battery pulse sequence:
        // Standard: 150ms ON, 200ms OFF
        // Low battery: 500ms ON, 200ms OFF
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
            // Battery display complete
            return_to_idle_or_advertising();
        }
        break;
    }

    case MODE_ADVERTISING:
        if (zmk_usb_is_powered()) {
            led_set(true);
            g_ind.mode = MODE_USB_POWERED;
            break;
        }
        // Periodic advertising pattern: 50ms ON, 100ms OFF, 50ms ON, 4800ms OFF (5s period)
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

    case MODE_USB_POWERED:
        led_set(true);
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

    // Automatically trigger BLE indicator on profile change!
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

    LOG_INF("Kugel indicator initialized successfully on P0.08");

    // Show initial state
    if (zmk_usb_is_powered()) {
        led_set(true);
        g_ind.mode = MODE_USB_POWERED;
    } else {
        kugel_indicator_trigger(IND_ALL);
    }

    return 0;
}

SYS_INIT(kugel_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
