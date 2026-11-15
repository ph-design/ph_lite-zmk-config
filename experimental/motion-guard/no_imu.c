/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zmk/motion_guard.h>
#include <zmk/events/position_state_changed.h>
#include "guard.h"
/* No sensor means no motion lock, no sampler and no startup settling. */
void zmk_motion_guard_key(uint32_t position, bool down, int64_t time) {
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL, .position = position,
        .state = down, .timestamp = time});
}
bool zmk_motion_guard_blocks_output(void) { return false; }
bool zmk_motion_guard_keeps_awake(void) { return false; }
bool zmk_motion_guard_clearing(void) { return false; }
uint32_t zmk_motion_guard_generation(void) { return 0; }
void zmk_motion_guard_prepare_sleep(void) {}
void zmk_motion_guard_resume_sampling(void) {}
void zmk_motion_guard_set_enabled(bool enabled) { ARG_UNUSED(enabled); }
int zmk_motion_guard_read_accel(struct sensor_value *xyz) { ARG_UNUSED(xyz); return -ENOTSUP; }
int zmk_motion_guard_set_tap_odr(bool enabled) { ARG_UNUSED(enabled); return -ENOTSUP; }
void zmk_motion_guard_get_config(struct mg_config *out) { *out = (struct mg_config)MG_CONFIG_DEFAULTS; }
int zmk_motion_guard_set_config(const struct mg_config *config) { ARG_UNUSED(config); return -ENOTSUP; }
int zmk_motion_guard_save_config(void) { return 0; }
int zmk_motion_guard_reset_config(void) { return 0; }
