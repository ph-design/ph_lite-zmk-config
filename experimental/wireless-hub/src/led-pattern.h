#pragma once
#include <stdbool.h>
#include <stdint.h>

// One GPIO LED: disconnected = 1 s on/off; low <=20%, clears at >=25%.
static inline bool low_battery_next(bool low, int percent) {
    if (percent < 0 || percent > 100) return false;
    return low ? percent < 25 : percent <= 20;
}
static inline bool dongle_led_on(uint32_t ms, bool connected, int percent, bool low) {
    uint32_t phase = ms % 2000;
    if (!connected) return phase < 1000;
    if (percent < 0) return phase < 100; // connected, telemetry unknown
    if (low) return phase < 100 || (phase >= 250 && phase < 350);
    return true;
}
