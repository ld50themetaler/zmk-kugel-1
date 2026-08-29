/*
 * Bit Trade One ADTB7M (PixArt PAW3204) Trackball Driver for ZMK
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_PAW3204_H_
#define ZEPHYR_DRIVERS_SENSOR_PAW3204_H_

#include <stdbool.h>
#include "paw3204_control.h"

void paw3204_set_scroll_mode(bool enable);
bool paw3204_get_scroll_mode(void);
void paw3204_set_speed_div(int div);
void paw3204_set_scroll_div(int div);

#endif /* ZEPHYR_DRIVERS_SENSOR_PAW3204_H_ */
