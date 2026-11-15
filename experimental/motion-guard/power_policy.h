/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct zmk_power_config {
    uint32_t idle_timeout_ms, rgb_off_ms, backlight_off_ms, deep_sleep_ms;
    bool on_usb, off_when_locked;
};
#define ZMK_POWER_DEFAULTS {120000, 30000, 30000, 0, false, true}
static inline bool power_config_valid(const struct zmk_power_config *c) {
    const uint32_t timers[] = {c->idle_timeout_ms, c->rgb_off_ms, c->backlight_off_ms, c->deep_sleep_ms};
    for (unsigned i = 0; i < 4; i++) {
        uint32_t min = i == 3 ? 60000 : 1000;
        if (timers[i] && (timers[i] < min || timers[i] > 86400000 || timers[i] % 1000)) return false;
    }
    return !c->deep_sleep_ms || (c->deep_sleep_ms >= c->idle_timeout_ms &&
        c->deep_sleep_ms >= c->rgb_off_ms && c->deep_sleep_ms >= c->backlight_off_ms);
}
struct power_decision { bool idle, rgb_off, backlight_off, deep_sleep; };
static inline struct power_decision power_decide(const struct zmk_power_config *c,
    uint64_t age, bool held, bool usb, bool locked, bool studio_unlocked) {
    bool timed = !held && (!usb || c->on_usb);
    bool lock_off = locked && c->off_when_locked;
    return (struct power_decision){
        .idle = locked || (timed && c->idle_timeout_ms && age >= c->idle_timeout_ms),
        .rgb_off = lock_off || (timed && c->rgb_off_ms && age >= c->rgb_off_ms),
        .backlight_off = lock_off || (timed && c->backlight_off_ms && age >= c->backlight_off_ms),
        .deep_sleep = !usb && !held && !locked && !studio_unlocked &&
            c->deep_sleep_ms && age >= c->deep_sleep_ms,
    };
}
