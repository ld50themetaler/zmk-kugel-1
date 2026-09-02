/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef DT_BINDINGS_ZMK_TRACKBALL_H_
#define DT_BINDINGS_ZMK_TRACKBALL_H_

#define TB_SPD_UP   1
#define TB_SPD_DN   2
#define TB_AM_TOG   3
#define TB_SCRL_TOG 4
#define TB_SCRL_MO  4
#define TB_SCRL_UP  5
#define TB_SCRL_DN  6
#define TB_ACCEL_TOG 7
#define TB_ROT_CW    8
#define TB_ROT_CCW   9
#define TB_ROT_RES   10

/* Aliases for convenience */
#define TB_ROT_R     TB_ROT_CW
#define TB_ROT_L     TB_ROT_CCW

#endif /* DT_BINDINGS_ZMK_TRACKBALL_H_ */
