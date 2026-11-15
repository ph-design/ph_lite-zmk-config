/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../../src/power_policy.h"
void zmk_power_get_config(struct zmk_power_config *out);
int zmk_power_set_config(const struct zmk_power_config *config);
int zmk_power_save_config(void);
int zmk_power_reset_config(void);
void zmk_power_note_activity(void);
void zmk_power_note_key(uint32_t position, bool down);
bool zmk_power_idle(void);
bool zmk_power_deep_sleep(void);
bool zmk_power_rgb_off(void);
bool zmk_power_backlight_off(void);
void zmk_power_update(void);

bool zmk_power_input_recent(uint32_t quiet_ms);
