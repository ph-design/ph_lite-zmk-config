/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdbool.h>
struct imu_game_config { uint32_t source, range, deadzone, smoothing; bool invert, swap; uint32_t flip; int32_t center_roll, center_pitch; };
#if defined(CONFIG_ZMK_MOTION_IMU_FLIP_Z) && CONFIG_ZMK_MOTION_IMU_FLIP_Z
#define IMU_BOARD_FLIP 4
#else
#define IMU_BOARD_FLIP 0
#endif
#define IMU_GAME_DEFAULTS {1,45,2,60,false,false,IMU_BOARD_FLIP,0,0}
struct imu_game_state { int32_t x,y,z,roll,pitch,axis; uint32_t time; bool valid, active, supported; };
bool zmk_imu_game_active(void);
void zmk_imu_game_view(bool enabled);
int zmk_imu_game_mode(bool enabled);
void zmk_imu_game_get_config(struct imu_game_config *out);
int zmk_imu_game_set_config(const struct imu_game_config *config);
void zmk_imu_game_state(struct imu_game_state *out);
int zmk_imu_game_save(void);
int zmk_imu_game_reset(void);
int zmk_usb_hid_send_axis(int16_t axis);
int zmk_2g4_send_axis(int16_t axis);
