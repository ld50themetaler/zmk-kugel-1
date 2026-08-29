/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT adns_adns7530

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/logging/log.h>
#include "adns7530.h"

LOG_MODULE_REGISTER(adns7530, LOG_LEVEL_DBG);

#define WRITE_TO(addr) (0x80 | (addr))
#define READ_FROM(addr) (addr)

/* Bit-bang / Software CS Controlled SPI for precise tSRAD timing */
static void cs_select(const struct gpio_dt_spec *cs, bool enable)
{
	if (cs->port != NULL) {
		gpio_pin_set_raw(cs->port, cs->pin, enable ? 0 : 1);
	}
}

static int adns7530_spi_write(const struct spi_dt_spec *spi, const uint8_t *data, size_t len)
{
	const struct spi_buf tx = { .buf = (void *)data, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	return spi_write_dt(spi, &tx_set);
}

static uint8_t adns7530_read_reg(const struct spi_dt_spec *spi, uint8_t reg)
{
	// 1. Send address
	uint8_t tx = reg & 0x7F;
	const struct spi_buf tx_buf = { .buf = &tx, .len = 1 };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	spi_write_dt(spi, &tx_set);

	// 2. Critical optical sensor tSRAD delay: 120 microseconds
	k_busy_wait(120);

	// 3. Read data byte
	uint8_t rx = 0;
	const struct spi_buf rx_buf = { .buf = &rx, .len = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	spi_read_dt(spi, &rx_set);

	return rx;
}

static void adns7530_poll(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct adns7530_data *data = CONTAINER_OF(dwork, struct adns7530_data, poll_work);
	const struct device *dev = data->dev;
	const struct adns7530_config *cfg = dev->config;

	// Burst read: 0x42 + tSRAD + 5 bytes (motion, x_low, y_low, xy_high, surface)
	uint8_t snd = 0x42;
	const struct spi_buf tx_buf = { .buf = &snd, .len = 1 };
	const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	spi_write_dt(&cfg->spi, &tx_set);

	k_busy_wait(120); // tSRAD

	uint8_t rcv[5] = {0};
	const struct spi_buf rx_buf = { .buf = rcv, .len = 5 };
	const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
	spi_read_dt(&cfg->spi, &rx_set);

	uint8_t motion = rcv[0];
	if ((motion & 0x80) && (motion != 0xFF)) {
		int16_t raw_x = ((int16_t)rcv[1] << 4) | (((int16_t)rcv[3] >> 4) << 12);
		int16_t raw_y = ((int16_t)rcv[2] << 4) | (((int16_t)rcv[3] & 0x0F) << 12);
		int16_t dx = -(raw_x >> 4);
		int16_t dy = -(raw_y >> 4);

		if (dx != 0 || dy != 0) {
			LOG_INF("ADNS Motion: dx=%d, dy=%d (motion=0x%02X)", dx, dy, motion);
			input_report_rel(dev, INPUT_REL_X, dx, false, K_NO_WAIT);
			input_report_rel(dev, INPUT_REL_Y, dy, true, K_NO_WAIT);
		}
	}

	k_work_reschedule(&data->poll_work, K_MSEC(8));
}

static int adns7530_init(const struct device *dev)
{
	struct adns7530_data *data = dev->data;
	const struct adns7530_config *cfg = dev->config;

	data->dev = dev;

	LOG_INF("=== ADNS-7530 Trackball Initializing with tSRAD delay ===");

	const struct device *gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

	// Drive P0.09 and P0.23 to LOW for Power ON
	if (gpio0 != NULL) {
		gpio_pin_configure(gpio0, 9, GPIO_OUTPUT_LOW);
		gpio_pin_configure(gpio0, 23, GPIO_OUTPUT_LOW);
	}
	k_msleep(150);

	// 1. Soft Reset & Power down
	{
		uint8_t snd[] = { WRITE_TO(0x3A), 0x5A, 0, 0, WRITE_TO(0x2E), 0, 0, 0 };
		adns7530_spi_write(&cfg->spi, snd, sizeof(snd));
	}
	k_msleep(50);

	// 2. Dummy read registers
	adns7530_read_reg(&cfg->spi, 0x02);
	adns7530_read_reg(&cfg->spi, 0x03);
	adns7530_read_reg(&cfg->spi, 0x04);
	adns7530_read_reg(&cfg->spi, 0x05);
	k_msleep(10);

	// 3. Laser optical initialization sequence
	{
		uint8_t seq[][2] = {
			{ WRITE_TO(0x3C), 0x27 },
			{ WRITE_TO(0x22), 0x0A },
			{ WRITE_TO(0x21), 0x01 },
			{ WRITE_TO(0x3C), 0x32 },
			{ WRITE_TO(0x23), 0x20 },
			{ WRITE_TO(0x3C), 0x05 },
		};
		for (int i = 0; i < ARRAY_SIZE(seq); i++) {
			adns7530_spi_write(&cfg->spi, seq[i], 2);
			k_usleep(50);
		}
	}
	k_msleep(10);

	// 4. Laser power configuration (optimal power 0x7F / 127)
	{
		uint8_t laser_power = 0x7F;
		uint8_t snd[] = {
			WRITE_TO(0x1A), 0x40,
			WRITE_TO(0x1F), (uint8_t)((~0x40) & 0xFF),
			WRITE_TO(0x1C), laser_power,
			WRITE_TO(0x1D), (uint8_t)(~laser_power)
		};
		adns7530_spi_write(&cfg->spi, snd, sizeof(snd));
	}
	k_msleep(10);

	// 5. Read PID with tSRAD delay
	uint8_t pid = adns7530_read_reg(&cfg->spi, 0x00);
	LOG_INF("ADNS-7530 Product ID with tSRAD: 0x%02X (expected 0x31)", pid);

	/* Start polling work */
	k_work_init_delayable(&data->poll_work, adns7530_poll);
	k_work_reschedule(&data->poll_work, K_MSEC(50));

	return 0;
}

#define ADNS7530_INIT(n)                                                                         \
	static struct adns7530_data adns7530_data_##n;                                               \
	static const struct adns7530_config adns7530_config_##n = {                                  \
		.spi = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA, 0), \
		.pow_gpio = GPIO_DT_SPEC_INST_GET_OR(n, pow_gpios, {0}),                                \
		.irq_gpio = GPIO_DT_SPEC_INST_GET_OR(n, irq_gpios, {0}),                                \
		.cpi = DT_INST_PROP_OR(n, cpi, 800),                                                     \
	};                                                                                           \
	DEVICE_DT_INST_DEFINE(n, adns7530_init, NULL, &adns7530_data_##n, &adns7530_config_##n,     \
			      POST_KERNEL, 95, NULL);

DT_INST_FOREACH_STATUS_OKAY(ADNS7530_INIT)
