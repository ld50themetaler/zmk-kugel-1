# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

zephyr_library()

zephyr_include_directories(include)
zephyr_include_directories(${ZEPHYR_BASE}/../zmk/app/include)

if(CONFIG_PAW3204)
    zephyr_library_sources(drivers/sensor/paw3204/paw3204.c)
    zephyr_library_sources(drivers/sensor/paw3204/paw3204_control.c)
    zephyr_library_sources(drivers/sensor/paw3204/behavior_trackball.c)
endif()

if(CONFIG_KSCAN_KUGEL)
    zephyr_library_sources(drivers/kscan/kscan_kugel.c)
endif()

if(CONFIG_KUGEL_INDICATOR)
    zephyr_library_sources(drivers/indicator/kugel_indicator.c)
endif()

if(CONFIG_ZMK_NON_LIPO_BATTERY_MANAGEMENT)
    zephyr_library_sources(drivers/battery/non_lipo_battery_management.c)
endif()
