/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/input/input.h>

#define ADNS7530_REG_PROD_ID     0x00
#define ADNS7530_REG_REV_ID      0x01
#define ADNS7530_REG_MOTION      0x02
#define ADNS7530_REG_DELTA_X     0x03
#define ADNS7530_REG_DELTA_Y     0x04
#define ADNS7530_REG_SQUAL       0x05
#define ADNS7530_REG_POWER_DOWN  0x06
#define ADNS7530_REG_CONFIG      0x0A

#define ADNS7530_MOTION_MOT      0x80

#define ADNS7530_CPI_400         0x00
#define ADNS7530_CPI_600         0x01
#define ADNS7530_CPI_800         0x02
#define ADNS7530_CPI_1000        0x03
#define ADNS7530_CPI_1200        0x04

struct adns7530_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec pow_gpio;
	struct gpio_dt_spec irq_gpio;
	uint16_t cpi;
};

struct adns7530_data {
	const struct device *dev;
	struct k_work_delayable poll_work;
	struct gpio_callback irq_cb;
};
