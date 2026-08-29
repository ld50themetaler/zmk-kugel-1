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
#include <zmk/ble.h>
#include <zmk/battery.h>
#include <zmk/activity.h>

LOG_MODULE_REGISTER(kugel_indicator, CONFIG_ZMK_LOG_LEVEL);

#define LED_NODE DT_ALIAS(led0)

#if !DT_NODE_EXISTS(LED_NODE)
#error "led0 alias is not defined in devicetree!"
#endif

static const struct gpio_dt_spec s_led = GPIO_DT_SPEC_GET(LED_NODE, gpios);

enum indicator_mode {
    MODE_IDLE,
    MODE_BATTERY,
    MODE_ADVERTISING,
    MODE_CONNECTED,
};

static struct {
    enum indicator_mode mode;
    uint8_t step;
    uint8_t pulses_remaining;
    bool ble_connected;
    bool is_sleeping;
    bool battery_shown;
    struct k_work_delayable work;
} g_ind;

static void led_set(bool on)
{
    if (s_led.port != NULL) {
        gpio_pin_set_dt(&s_led, on ? 1 : 0);
    }
}

static void trigger_advertising(void);

static void indicator_work_handler(struct k_work *work)
{
    if (g_ind.is_sleeping) {
        led_set(false);
        g_ind.mode = MODE_IDLE;
        return;
    }

    switch (g_ind.mode) {
    case MODE_BATTERY:
        // Battery pulse sequence: 100ms ON, 200ms OFF for N pulses
        if (g_ind.step % 2 == 0) {
            // Turn ON
            led_set(true);
            g_ind.step++;
            k_work_reschedule(&g_ind.work, K_MSEC(100));
        } else {
            // Turn OFF
            led_set(false);
            g_ind.step++;
            if (g_ind.pulses_remaining > 1) {
                g_ind.pulses_remaining--;
                k_work_reschedule(&g_ind.work, K_MSEC(200));
            } else {
                // Finished battery display
                g_ind.mode = MODE_IDLE;
                g_ind.battery_shown = true;
                // Transition to BLE state
                if (zmk_ble_active_profile_is_connected()) {
                    g_ind.mode = MODE_IDLE;
                } else {
                    trigger_advertising();
                }
            }
        }
        break;

    case MODE_CONNECTED:
        // Turn OFF after 300ms single flash
        led_set(false);
        g_ind.mode = MODE_IDLE;
        break;

    case MODE_ADVERTISING:
        // Advertising pattern: 50ms ON, 100ms OFF, 50ms ON, 4800ms OFF (5s period)
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
            // Sleep for remainder of 5000ms period (5000 - 200 = 4800ms)
            k_work_reschedule(&g_ind.work, K_MSEC(4800));
            break;
        }
        break;

    case MODE_IDLE:
    default:
        led_set(false);
        break;
    }
}

static void trigger_battery_display(uint8_t soc)
{
    // QMK logic: > 70% -> 3 pulses, 30%..70% -> 2 pulses, < 30% -> 1 pulse
    uint8_t pulses;
    if (soc > 70) {
        pulses = 3;
    } else if (soc > 30) {
        pulses = 2;
    } else {
        pulses = 1;
    }

    LOG_INF("Triggering battery indicator: SOC=%d%%, pulses=%d", soc, pulses);
    g_ind.mode = MODE_BATTERY;
    g_ind.step = 0;
    g_ind.pulses_remaining = pulses;
    k_work_reschedule(&g_ind.work, K_MSEC(10));
}

static void trigger_connected(void)
{
    LOG_INF("Triggering connected indicator (300ms single flash)");
    g_ind.mode = MODE_CONNECTED;
    g_ind.step = 0;
    led_set(true);
    k_work_reschedule(&g_ind.work, K_MSEC(300));
}

static void trigger_advertising(void)
{
    if (g_ind.mode == MODE_BATTERY) {
        return; // Let battery finish first
    }
    LOG_INF("Triggering advertising indicator (5s double-pulse)");
    g_ind.mode = MODE_ADVERTISING;
    g_ind.step = 0;
    k_work_reschedule(&g_ind.work, K_MSEC(100));
}

static int ble_profile_listener(const zmk_event_t *eh)
{
    const struct zmk_ble_active_profile_changed *ev = as_zmk_ble_active_profile_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool is_connected = zmk_ble_active_profile_is_connected();
    LOG_INF("BLE profile changed: index=%d, connected=%d", ev->index, is_connected);

    if (is_connected && !g_ind.ble_connected) {
        g_ind.ble_connected = true;
        trigger_connected();
    } else if (!is_connected) {
        g_ind.ble_connected = false;
        trigger_advertising();
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_ble_indicator, ble_profile_listener);
ZMK_SUBSCRIPTION(kugel_ble_indicator, zmk_ble_active_profile_changed);

static int battery_state_listener(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_INF("Battery state changed: SOC=%d%%", ev->state_of_charge);

    if (!g_ind.battery_shown) {
        trigger_battery_display(ev->state_of_charge);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(kugel_battery_indicator, battery_state_listener);
ZMK_SUBSCRIPTION(kugel_battery_indicator, zmk_battery_state_changed);

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
        led_set(false);
        g_ind.mode = MODE_IDLE;
    } else if (ev->state == ZMK_ACTIVITY_ACTIVE && g_ind.is_sleeping) {
        LOG_INF("Waking up from sleep");
        g_ind.is_sleeping = false;
        if (!zmk_ble_active_profile_is_connected()) {
            trigger_advertising();
        }
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

    LOG_INF("Kugel indicator initialized successfully on P0.08");

    uint8_t initial_soc = zmk_battery_state_of_charge();
    if (initial_soc > 0) {
        trigger_battery_display(initial_soc);
    } else {
        k_work_reschedule(&g_ind.work, K_MSEC(800));
    }

    return 0;
}

SYS_INIT(kugel_indicator_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
