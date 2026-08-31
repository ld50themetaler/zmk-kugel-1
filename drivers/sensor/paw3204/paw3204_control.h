/*
 * Bit Trade One ADTB7M (PAW3204) Advanced Trackball Control Subsystem
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef ZEPHYR_DRIVERS_SENSOR_PAW3204_CONTROL_H_
#define ZEPHYR_DRIVERS_SENSOR_PAW3204_CONTROL_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Layer IDs (must match kugel.keymap)
#define MOUSE_LAYER_ID 4
#define SNIPE_LAYER_ID 5

// Initialization
void paw3204_control_init(void);

// Motion Hook: called on each motion event from paw3204.c
void paw3204_control_on_motion(int8_t dx, int8_t dy);

// Speed & Mode Queries
void paw3204_control_calculate_motion(int8_t dx, int8_t dy, int *out_dx, int *out_dy);
int paw3204_control_get_effective_div(int8_t dx, int8_t dy);
int paw3204_control_get_scroll_div(void);
bool paw3204_control_is_sniper_active(void);
bool paw3204_control_is_scroll_mode(void);
void paw3204_control_set_scroll_mode(bool enable);
void paw3204_control_toggle_scroll_mode(void);

// Dynamic Pointer Speed Control (Level 1: 2.0x, Level 2: 1.5x, Level 3: 1.0x, Level 4: 0.7x, Level 5: 0.5x, Level 6: 0.25x)
void paw3204_control_speed_up(void);
void paw3204_control_speed_down(void);
uint8_t paw3204_control_get_speed_level(void);

// Dynamic Scroll Sensitivity Control (Level 1: div 36 (Slow/Smooth) to Level 6: div 8 (Fast))
void paw3204_control_scroll_speed_up(void);
void paw3204_control_scroll_speed_down(void);
uint8_t paw3204_control_get_scroll_level(void);

// Auto-Mouse Layer Control
void paw3204_control_toggle_automouse(void);
bool paw3204_control_is_automouse_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_SENSOR_PAW3204_CONTROL_H_ */
