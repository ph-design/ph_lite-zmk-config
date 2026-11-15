/* SPDX-License-Identifier: MIT */
#pragma once
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zmk/motion.h>
struct zmk_tap_options { uint32_t axes, quiet_ms; bool split; };
struct zmk_tap_diagnostics {
    uint32_t irq, single, dbl, gestures, actions, suppressed, drops;
    uint32_t axes[3], src, gesture, odr, test_ms, engine, full_scale_mg;
    uint8_t registers[11];
    int32_t error, current[3];
};
struct zmk_tap_capture { uint32_t sequence, count; uint8_t frames[256]; };
void zmk_tap_get_options(struct zmk_tap_options *out);
bool zmk_tap_options_valid(const struct zmk_tap_options *o);
int zmk_tap_set_options(const struct zmk_tap_options *o);
int zmk_tap_rearm(const struct device *imu, const struct zmk_motion_tap_config *cfg);
int zmk_tap_save_options(void);
int zmk_tap_reset(void);
void zmk_tap_get_diagnostics(struct zmk_tap_diagnostics *out);
int zmk_tap_set_test(bool enabled);
bool zmk_tap_test_active(void);
void zmk_tap_sample(const struct sensor_value *xyz);
void zmk_tap_get_capture(struct zmk_tap_capture *out);
/* Implemented next to the driver's CLICK_SRC read, never re-read by RPC. */
void zmk_tap_driver_irq(uint8_t src);
void zmk_tap_driver_registers(const uint8_t *registers, uint32_t odr, uint32_t fs_mg);
int zmk_tap_sensor_configure(const struct device *dev, uint8_t axes, bool dbl);

void zmk_tap_driver_gpio(void);
