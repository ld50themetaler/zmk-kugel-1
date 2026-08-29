/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_kugel

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <zephyr/logging/log.h>

#include "../sensor/paw3204/paw3204.h"

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_REGISTER(kscan_kugel, LOG_LEVEL_INF);

// BLE Micro Pro Onboard Blue LED (P0.15, Active LOW)
#define BMP_LED_PIN 15

struct kscan_kugel_config {
	struct spi_dt_spec spi;
	struct gpio_dt_spec reset_gpio;
	struct gpio_dt_spec row_gpio;
	struct gpio_dt_spec int_gpio;
};

struct kscan_kugel_data {
	const struct device *dev;
	const struct device *gpio0_dev;
	const struct device *cdc_dev;
	kscan_callback_t callback;
	struct k_work_delayable work;
	struct k_work_delayable led_work;
	uint16_t last_raw[3];
	uint32_t scan_count;
	bool settings_loaded;
	bool is_connected;
	int led_blink_count;
};

static struct kscan_kugel_data *g_kscan_data = NULL;

static void bmp_led_set(struct kscan_kugel_data *data, bool on)
{
	if (data->gpio0_dev && device_is_ready(data->gpio0_dev)) {
		// Active LOW: 0 = ON (Blue Lit), 1 = OFF
		gpio_pin_set_raw(data->gpio0_dev, BMP_LED_PIN, on ? 0 : 1);
	}
}

static void led_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct kscan_kugel_data *data = CONTAINER_OF(dwork, struct kscan_kugel_data, led_work);

	if (data->led_blink_count > 0) {
		// Boot sequence: 3 clean blinks
		bool on = (data->led_blink_count % 2 != 0);
		bmp_led_set(data, on);
		data->led_blink_count--;
		k_work_reschedule(&data->led_work, K_MSEC(120));
		return;
	}

	if (!data->is_connected) {
		// Advertising (Pairing / Searching): 1s blink cycle
		static bool toggle = false;
		toggle = !toggle;
		bmp_led_set(data, toggle);
		k_work_reschedule(&data->led_work, K_MSEC(toggle ? 100 : 900));
	} else {
		// Connected: Completely OFF
		bmp_led_set(data, false);
	}
}

static void ble_connected(struct bt_conn *conn, uint8_t err)
{
	if (err == 0 && g_kscan_data) {
		g_kscan_data->is_connected = true;
		k_work_reschedule(&g_kscan_data->led_work, K_NO_WAIT);
	}
}

static void ble_disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (g_kscan_data) {
		g_kscan_data->is_connected = false;
		k_work_reschedule(&g_kscan_data->led_work, K_NO_WAIT);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = ble_connected,
	.disconnected = ble_disconnected,
};

static int mcp23s17_transfer(const struct spi_dt_spec *spi, const uint8_t *tx_buf, uint8_t *rx_buf, size_t len)
{
	const struct spi_buf tx = { .buf = (void *)tx_buf, .len = len };
	const struct spi_buf rx = { .buf = rx_buf, .len = len };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	const struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };

	return spi_transceive_dt(spi, &tx_set, &rx_set);
}

static void check_touch_reset(struct kscan_kugel_data *data)
{
	if (!data->cdc_dev || !device_is_ready(data->cdc_dev)) {
		data->cdc_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
		if (!data->cdc_dev) {
			data->cdc_dev = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zmk_studio_rpc_uart));
		}
	}

	if (data->cdc_dev && device_is_ready(data->cdc_dev)) {
		uint32_t baudrate = 0;
		if (uart_line_ctrl_get(data->cdc_dev, UART_LINE_CTRL_BAUD_RATE, &baudrate) == 0) {
			if (baudrate == 1200) {
				LOG_INF("1200bps touch reset detected! Jumping to BLE Micro Pro bootloader...");
				nrf_power_gpregret_set(NRF_POWER, 0, 0x57);
				k_msleep(50);
				sys_reboot(SYS_REBOOT_COLD);
			}
		}
	}
}

static void kscan_kugel_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct kscan_kugel_data *data = CONTAINER_OF(dwork, struct kscan_kugel_data, work);
	const struct kscan_kugel_config *cfg = data->dev->config;

#if IS_ENABLED(CONFIG_SETTINGS)
	if (!data->settings_loaded) {
		settings_load();
		data->settings_loaded = true;
	}
#endif

	check_touch_reset(data);

	uint16_t current_raw[3] = { 0xFFFF, 0xFFFF, 0xFFFF };

	for (uint8_t chip = 0; chip < 3; chip++) {
		uint8_t tx[4] = { (uint8_t)(0x40 | (chip << 1) | 0x01), 0x12, 0, 0 };
		uint8_t rx[4] = { 0 };
		if (mcp23s17_transfer(&cfg->spi, tx, rx, 4) == 0) {
			current_raw[chip] = (uint16_t)rx[2] | ((uint16_t)rx[3] << 8);
		}

		uint16_t changed = current_raw[chip] ^ data->last_raw[chip];
		if (changed != 0) {
			for (uint8_t pin = 0; pin < 16; pin++) {
				if (changed & (1U << pin)) {
					bool pressed = ((current_raw[chip] & (1U << pin)) == 0);

					// Dynamic Scroll Layer Trigger: Raise (Chip 1, Pin 6 / RC(1,6))
					if (chip == 1 && pin == 6) {
						paw3204_set_scroll_mode(pressed);
					}

					if (data->callback) {
						data->callback(data->dev, chip, pin, pressed);
					}
				}
			}
			data->last_raw[chip] = current_raw[chip];
		}
	}

	k_work_reschedule(&data->work, K_MSEC(5));
}

static int kscan_kugel_configure(const struct device *dev, kscan_callback_t callback)
{
	struct kscan_kugel_data *data = dev->data;
	if (!callback) {
		return -EINVAL;
	}
	data->callback = callback;
	return 0;
}

static int kscan_kugel_enable_callback(const struct device *dev)
{
	struct kscan_kugel_data *data = dev->data;
	k_work_reschedule(&data->work, K_NO_WAIT);
	return 0;
}

static int kscan_kugel_disable_callback(const struct device *dev)
{
	struct kscan_kugel_data *data = dev->data;
	k_work_cancel_delayable(&data->work);
	return 0;
}

static const struct kscan_driver_api kscan_kugel_api = {
	.config = kscan_kugel_configure,
	.enable_callback = kscan_kugel_enable_callback,
	.disable_callback = kscan_kugel_disable_callback,
};

static int kscan_kugel_init(const struct device *dev)
{
	struct kscan_kugel_data *data = dev->data;
	const struct kscan_kugel_config *cfg = dev->config;

	data->dev = dev;
	data->cdc_dev = NULL;
	data->last_raw[0] = 0xFFFF;
	data->last_raw[1] = 0xFFFF;
	data->last_raw[2] = 0xFFFF;
	data->scan_count = 0;
	data->settings_loaded = false;
	data->is_connected = false;
	data->led_blink_count = 6; // 3 clean blinks
	g_kscan_data = data;

	LOG_INF("Initializing Kugel-1 MCP23S17 Kscan driver...");

	// Initialize BLE Micro Pro Onboard Blue LED (P0.15)
	data->gpio0_dev = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (data->gpio0_dev && device_is_ready(data->gpio0_dev)) {
		gpio_pin_configure(data->gpio0_dev, BMP_LED_PIN, GPIO_OUTPUT | GPIO_OUTPUT_INACTIVE);
	}

	if (!spi_is_ready_dt(&cfg->spi)) {
		LOG_ERR("SPI bus is not ready!");
		return -ENODEV;
	}

	// 1. Hardware Reset of MCP23S17
	if (cfg->reset_gpio.port != NULL && gpio_is_ready_dt(&cfg->reset_gpio)) {
		gpio_pin_configure_dt(&cfg->reset_gpio, GPIO_OUTPUT);
		gpio_pin_set_raw(cfg->reset_gpio.port, cfg->reset_gpio.pin, 0);
		k_msleep(10);
		gpio_pin_set_raw(cfg->reset_gpio.port, cfg->reset_gpio.pin, 1);
		k_msleep(20);
	}

	// 2. Drive common row ground (P0.22) to LOW
	if (cfg->row_gpio.port != NULL && gpio_is_ready_dt(&cfg->row_gpio)) {
		gpio_pin_configure_dt(&cfg->row_gpio, GPIO_OUTPUT);
		gpio_pin_set_raw(cfg->row_gpio.port, cfg->row_gpio.pin, 0);
	}

	// 3. MCP23S17 Initialization
	uint8_t rx_dummy[4];
	{
		uint8_t tx[] = { 0x40, 0x0C, 0xFF, 0xFF };
		mcp23s17_transfer(&cfg->spi, tx, rx_dummy, sizeof(tx));
	}
	k_msleep(2);
	{
		uint8_t tx[] = { 0x40, 0x04, 0xFF, 0xFF };
		mcp23s17_transfer(&cfg->spi, tx, rx_dummy, sizeof(tx));
	}
	k_msleep(2);
	{
		uint8_t tx[] = { 0x40, 0x0A, 0x4C };
		mcp23s17_transfer(&cfg->spi, tx, rx_dummy, sizeof(tx));
	}
	k_msleep(5);

	for (uint8_t chip = 0; chip < 3; chip++) {
		uint8_t tx_gppu[] = { (uint8_t)(0x40 | (chip << 1)), 0x0C, 0xFF, 0xFF };
		mcp23s17_transfer(&cfg->spi, tx_gppu, rx_dummy, sizeof(tx_gppu));

		uint8_t tx_iodir[] = { (uint8_t)(0x40 | (chip << 1)), 0x00, 0xFF, 0xFF };
		mcp23s17_transfer(&cfg->spi, tx_iodir, rx_dummy, sizeof(tx_iodir));
	}

	LOG_INF("Kugel-1 initialized successfully");

	k_work_init_delayable(&data->work, kscan_kugel_work_handler);
	k_work_init_delayable(&data->led_work, led_work_handler);

	k_work_reschedule(&data->led_work, K_NO_WAIT);

	return 0;
}

#define KSCAN_KUGEL_INIT(n)                                                                      \
	static struct kscan_kugel_data kscan_kugel_data_##n;                                         \
	static const struct kscan_kugel_config kscan_kugel_config_##n = {                             \
		.spi = SPI_DT_SPEC_INST_GET(n, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_MODE_CPOL | SPI_MODE_CPHA, 0), \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                            \
		.row_gpio = GPIO_DT_SPEC_INST_GET_OR(n, row_gpios, {0}),                                \
		.int_gpio = GPIO_DT_SPEC_INST_GET_OR(n, int_gpios, {0}),                                \
	};                                                                                           \
	DEVICE_DT_INST_DEFINE(n, kscan_kugel_init, NULL, &kscan_kugel_data_##n,                      \
			      &kscan_kugel_config_##n, POST_KERNEL, 90, &kscan_kugel_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_KUGEL_INIT)
