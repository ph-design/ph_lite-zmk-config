/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct sensor_value;
int zmk_motion_guard_read_accel(struct sensor_value *xyz);
void zmk_motion_guard_key(uint32_t position, bool down, int64_t time);
void zmk_motion_guard_set_enabled(bool enabled);
bool zmk_motion_guard_blocks_output(void);
bool zmk_motion_guard_keeps_awake(void);
bool zmk_motion_guard_clearing(void);
uint32_t zmk_motion_guard_generation(void);

#include <stdint.h>
struct mg_config;
void zmk_motion_guard_get_config(struct mg_config *config);
int zmk_motion_guard_set_config(const struct mg_config *config);
int zmk_motion_guard_save_config(void);
int zmk_motion_guard_reset_config(void);
