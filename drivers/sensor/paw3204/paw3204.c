/*
 * Bit Trade One ADTB7M (PixArt PAW3204) Trackball Driver for ZMK
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT bto_paw3204

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include "paw3204.h"

LOG_MODULE_REGISTER(paw3204, LOG_LEVEL_INF);

#define REG_PID1 0x00
#define REG_PID2 0x01
#define REG_STAT 0x02
#define REG_X    0x03
#define REG_Y    0x04

#define READ(addr)  (addr)
#define WRITE(addr) (0x80 | (addr))

struct paw3204_config {
	struct gpio_dt_spec sclk_gpio;
	struct gpio_dt_spec data_gpio;
	struct gpio_dt_spec cs_gpio;
	struct gpio_dt_spec pow_gpio;
};

struct paw3204_data {
	const struct device *dev;
	struct k_work_delayable poll_work;
	bool scroll_mode;
	int speed_div;
	int scroll_div;
	int scroll_y_accum;
	int scroll_x_accum;
	uint32_t idle_count;
};

static struct paw3204_data *g_paw_data = NULL;

void paw3204_set_scroll_mode(bool enable)
{
	paw3204_control_set_scroll_mode(enable);
	if (g_paw_data) {
		g_paw_data->scroll_mode = enable;
		g_paw_data->scroll_y_accum = 0;
		g_paw_data->scroll_x_accum = 0;
	}
}

bool paw3204_get_scroll_mode(void)
{
	return paw3204_control_is_scroll_mode() || (g_paw_data ? g_paw_data->scroll_mode : false);
}

void paw3204_set_speed_div(int div)
{
	if (g_paw_data && div > 0) {
		g_paw_data->speed_div = div;
	}
}

void paw3204_set_scroll_div(int div)
{
	if (g_paw_data && div > 0) {
		g_paw_data->scroll_div = div;
	}
}

static void paw3204_write_byte(const struct paw3204_config *cfg, uint8_t byte)
{
	gpio_pin_configure_dt(&cfg->data_gpio, GPIO_OUTPUT);
	for (int i = 7; i >= 0; i--) {
		gpio_pin_set_dt(&cfg->sclk_gpio, 0); // SCLK LOW
		gpio_pin_set_dt(&cfg->data_gpio, (byte >> i) & 1);
		k_busy_wait(2);
		gpio_pin_set_dt(&cfg->sclk_gpio, 1); // SCLK HIGH
		k_busy_wait(2);
	}
}

static uint8_t paw3204_read_byte(const struct paw3204_config *cfg)
{
	gpio_pin_configure_dt(&cfg->data_gpio, GPIO_INPUT | GPIO_PULL_UP);
	k_busy_wait(5); // Turnaround delay

	uint8_t byte = 0;
	for (int i = 7; i >= 0; i--) {
		gpio_pin_set_dt(&cfg->sclk_gpio, 0); // SCLK LOW
		k_busy_wait(2);
		gpio_pin_set_dt(&cfg->sclk_gpio, 1); // SCLK HIGH
		k_busy_wait(2);
		if (gpio_pin_get_dt(&cfg->data_gpio) > 0) {
			byte |= (1 << i);
		}
	}
	return byte;
}

static uint8_t paw3204_read_reg(const struct paw3204_config *cfg, uint8_t addr)
{
	if (cfg->cs_gpio.port != NULL) {
		gpio_pin_set_dt(&cfg->cs_gpio, 0);
		k_busy_wait(2);
	}

	paw3204_write_byte(cfg, READ(addr));
	uint8_t val = paw3204_read_byte(cfg);

	if (cfg->cs_gpio.port != NULL) {
		gpio_pin_set_dt(&cfg->cs_gpio, 1);
	}

	return val;
}

static void paw3204_write_reg(const struct paw3204_config *cfg, uint8_t addr, uint8_t val)
{
	if (cfg->cs_gpio.port != NULL) {
		gpio_pin_set_dt(&cfg->cs_gpio, 0);
		k_busy_wait(2);
	}

	paw3204_write_byte(cfg, WRITE(addr));
	paw3204_write_byte(cfg, val);

	if (cfg->cs_gpio.port != NULL) {
		gpio_pin_set_dt(&cfg->cs_gpio, 1);
	}
}

static void paw3204_poll(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct paw3204_data *data = CONTAINER_OF(dwork, struct paw3204_data, poll_work);
	const struct device *dev = data->dev;
	const struct paw3204_config *cfg = dev->config;

	uint8_t stat = paw3204_read_reg(cfg, REG_STAT);

	if ((stat & 0x80) && stat != 0xFF) {
		int8_t raw_x = (int8_t)paw3204_read_reg(cfg, REG_X);
		int8_t raw_y = (int8_t)paw3204_read_reg(cfg, REG_Y);

		// Recover data line by dummy write
		paw3204_write_reg(cfg, 0x00, 0xFF);

		// 90-degree rotational compensation
		int8_t dx = -raw_y;
		int8_t dy = raw_x;

		if (dx != 0 || dy != 0) {
			// Notify control subsystem (triggers auto-mouse layer timer)
			paw3204_control_on_motion(dx, dy);

			bool in_scroll = data->scroll_mode || paw3204_control_is_scroll_mode();

			if (in_scroll) {
				// Scroll Mode with Axis Lock:
				// Down (+dy) scrolls down (-INPUT_REL_WHEEL)
				// Prevent accidental horizontal scroll during vertical scrolling
				if (abs(dy) >= 2 * abs(dx)) {
					data->scroll_y_accum += dy;
				} else if (abs(dx) >= 2 * abs(dy)) {
					data->scroll_x_accum += dx;
				} else {
					data->scroll_y_accum += dy;
					data->scroll_x_accum += dx;
				}

				int div = paw3204_control_get_scroll_div();
				if (div <= 0) div = 20;

				if (abs(data->scroll_y_accum) >= div) {
					int wheel_steps = -(data->scroll_y_accum / div);
					input_report_rel(dev, INPUT_REL_WHEEL, wheel_steps, true, K_NO_WAIT);
					data->scroll_y_accum %= div;
				}

				if (abs(data->scroll_x_accum) >= div) {
					int hwheel_steps = (data->scroll_x_accum / div);
					input_report_rel(dev, INPUT_REL_HWHEEL, hwheel_steps, true, K_NO_WAIT);
					data->scroll_x_accum %= div;
				}
			} else {
				// Normal Cursor Move Mode with Dynamic Speed Levels & Acceleration
				int final_dx = 0, final_dy = 0;
				paw3204_control_calculate_motion(dx, dy, &final_dx, &final_dy);

				input_report_rel(dev, INPUT_REL_X, final_dx, false, K_NO_WAIT);
				input_report_rel(dev, INPUT_REL_Y, final_dy, true, K_NO_WAIT);
			}
			data->idle_count = 0;
		} else {
			if (data->idle_count < 100) {
				data->idle_count++;
			}
		}
	} else {
		if (data->idle_count < 100) {
			data->idle_count++;
		}
	}

	uint32_t next_poll_ms = (data->idle_count >= 20) ? 25 : 8;
	k_work_reschedule(&data->poll_work, K_MSEC(next_poll_ms));
}

static int paw3204_init(const struct device *dev)
{
	struct paw3204_data *data = dev->data;
	const struct paw3204_config *cfg = dev->config;

	data->dev = dev;
	data->scroll_mode = false;
	data->speed_div = 2;   // Divisor 2: 50% speed for precise control
	data->scroll_div = 6;  // Divisor 6: Smooth vertical scrolling
	data->scroll_y_accum = 0;
	data->scroll_x_accum = 0;
	data->idle_count = 0;
	g_paw_data = data;

	LOG_INF("Initializing Bit Trade One ADTB7M (PAW3204) Trackball Driver...");

	paw3204_control_init();

	// Power ON (TB_POW / P0.23 LOW)
	if (cfg->pow_gpio.port != NULL && gpio_is_ready_dt(&cfg->pow_gpio)) {
		gpio_pin_configure_dt(&cfg->pow_gpio, GPIO_OUTPUT_LOW);
		LOG_INF("ADTB7M Power ON (P0.23 LOW)");
	}

	// CS (P0.09 HIGH)
	if (cfg->cs_gpio.port != NULL && gpio_is_ready_dt(&cfg->cs_gpio)) {
		gpio_pin_configure_dt(&cfg->cs_gpio, GPIO_OUTPUT_HIGH);
	}

	if (!gpio_is_ready_dt(&cfg->sclk_gpio) || !gpio_is_ready_dt(&cfg->data_gpio)) {
		LOG_ERR("PAW3204 GPIO not ready!");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&cfg->sclk_gpio, GPIO_OUTPUT_HIGH);
	gpio_pin_configure_dt(&cfg->data_gpio, GPIO_OUTPUT_HIGH);
	k_msleep(100);

	// Read Product ID
	uint8_t pid = paw3204_read_reg(cfg, REG_PID1);
	LOG_INF("Bit Trade One ADTB7M (PAW3204) Product ID: 0x%02X (expected 0x30)", pid);

	// Safely configure Hardware Resolution to 1600 CPI (Reg 0x06 Configuration, bits [2:0] = 110b)
	uint8_t cfg_val = paw3204_read_reg(cfg, 0x06);
	if (cfg_val != 0xFF) {
		cfg_val = (cfg_val & ~0x07) | 0x06; // Preserve bits [7:3], set CPI[2:0] = 110b (1600 CPI)
		paw3204_write_reg(cfg, 0x06, cfg_val);
		LOG_INF("PAW3204 Hardware Resolution configured to 1600 CPI (Reg 0x06 = 0x%02X)", cfg_val);
	} else {
		LOG_WRN("PAW3204 Reg 0x06 read failed (0xFF), retaining default CPI");
	}

	// Recover data line
	paw3204_write_reg(cfg, 0x00, 0xFF);

	k_work_init_delayable(&data->poll_work, paw3204_poll);
	k_work_reschedule(&data->poll_work, K_MSEC(50));

	return 0;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)
static int paw3204_pm_action(const struct device *dev, enum pm_device_action action)
{
	struct paw3204_data *data = dev->data;
	const struct paw3204_config *cfg = dev->config;

	switch (action) {
	case PM_DEVICE_ACTION_SUSPEND:
		k_work_cancel_delayable(&data->poll_work);
		if (cfg->pow_gpio.port != NULL && gpio_is_ready_dt(&cfg->pow_gpio)) {
			gpio_pin_set_dt(&cfg->pow_gpio, 1); // Power OFF (Active LOW)
		}
		LOG_INF("PAW3204 suspended, power OFF");
		break;
	case PM_DEVICE_ACTION_RESUME:
		if (cfg->pow_gpio.port != NULL && gpio_is_ready_dt(&cfg->pow_gpio)) {
			gpio_pin_set_dt(&cfg->pow_gpio, 0); // Power ON (Active LOW)
			k_msleep(50);
		}
		paw3204_write_reg(cfg, 0x00, 0xFF);
		data->idle_count = 0;
		k_work_reschedule(&data->poll_work, K_MSEC(50));
		LOG_INF("PAW3204 resumed, power ON");
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}
#endif

#define PAW3204_INIT(n)                                                                          \
	static struct paw3204_data paw3204_data_##n;                                                 \
	static const struct paw3204_config paw3204_config_##n = {                                    \
		.sclk_gpio = GPIO_DT_SPEC_INST_GET(n, sclk_gpios),                                       \
		.data_gpio = GPIO_DT_SPEC_INST_GET(n, data_gpios),                                       \
		.cs_gpio = GPIO_DT_SPEC_INST_GET_OR(n, cs_gpios, {0}),                                   \
		.pow_gpio = GPIO_DT_SPEC_INST_GET_OR(n, pow_gpios, {0}),                                 \
	};                                                                                           \
	PM_DEVICE_DT_INST_DEFINE(n, paw3204_pm_action);                                              \
	DEVICE_DT_INST_DEFINE(n, paw3204_init, PM_DEVICE_DT_INST_GET(n), &paw3204_data_##n,          \
			      &paw3204_config_##n, POST_KERNEL, 95, NULL);

DT_INST_FOREACH_STATUS_OKAY(PAW3204_INIT)
