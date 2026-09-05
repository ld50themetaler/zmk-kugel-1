/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <dt-bindings/zmk/indicator.h>

void kugel_indicator_trigger(uint8_t mode);
void kugel_indicator_key_press(void);
void kugel_indicator_mouse_layer_changed(bool active);
void kugel_indicator_cycle_am_led_mode(void);
uint8_t kugel_indicator_get_am_led_mode(void);
