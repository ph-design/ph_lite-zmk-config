/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

int zmk_2g4_send_keyboard_report(void);
int zmk_2g4_send_consumer_report(void);
#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_2g4_send_mouse_report(void);
#endif
bool zmk_2g4_is_ready(void);
int zmk_2g4_start(void);
int zmk_2g4_stop(void);
bool zmk_2g4_dongle_kb_connected(void);
void zmk_2g4_dongle_rx_stats(uint32_t *rx_total, uint32_t *decrypt_fail);
// Returns 0..100, or -1 when disconnected/unknown/stale.
int zmk_2g4_dongle_battery_percent(void);
int zmk_2g4_dongle_start(void);
