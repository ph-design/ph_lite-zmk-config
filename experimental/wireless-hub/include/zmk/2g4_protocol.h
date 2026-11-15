/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

// Device to Dongle
#define ZMK_2G4_MSG_KEYBOARD_REPORT 0x01
#define ZMK_2G4_MSG_CONSUMER_REPORT 0x02
#define ZMK_2G4_MSG_MOUSE_REPORT 0x03
#define ZMK_2G4_MSG_BOOT 0x05
#define ZMK_2G4_MSG_KEEP_ALIVE 0xFE

// Dongle to Device
#define ZMK_2G4_ACK_LED_INDICATORS 0x01
