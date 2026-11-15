/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "power_policy.h"
int main(void) {
    struct zmk_power_config c = ZMK_POWER_DEFAULTS;
    assert(power_config_valid(&c));
    struct power_decision d = power_decide(&c, 0, false, false, false, false);
    assert(!d.idle && !d.rgb_off && !d.deep_sleep);
    d = power_decide(&c, 30000, false, false, false, false);
    assert(d.rgb_off && d.backlight_off && !d.idle && !d.deep_sleep);
    d = power_decide(&c, 120000, false, false, false, false);
    assert(d.idle && d.rgb_off && !d.deep_sleep);
    d = power_decide(&c, 90000000ULL, false, false, false, false);
    assert(d.idle && !d.deep_sleep); /* no automatic System OFF by default */
    d = power_decide(&c, 90000000ULL, true, false, false, false);
    assert(!d.idle && !d.rgb_off && !d.deep_sleep); /* held game key */
    d = power_decide(&c, 90000000ULL, false, true, false, false);
    assert(!d.idle && !d.rgb_off && !d.deep_sleep);
    c.on_usb = true;
    d = power_decide(&c, 120000, false, true, false, false);
    assert(d.idle && d.rgb_off && !d.deep_sleep);
    d = power_decide(&c, 0, false, true, true, false);
    assert(d.idle && d.rgb_off); /* locking has its own light setting */
    c.off_when_locked = false;
    d = power_decide(&c, 0, false, false, true, false);
    assert(d.idle && !d.rgb_off);
    c.deep_sleep_ms = 1800000;
    assert(power_config_valid(&c));
    assert(!power_decide(&c, 1799999, false, false, false, false).deep_sleep);
    assert(power_decide(&c, 1800000, false, false, false, false).deep_sleep);
    assert(!power_decide(&c, 1800000, false, false, true, false).deep_sleep);
    assert(!power_decide(&c, 1800000, true, false, false, false).deep_sleep);
    assert(!power_decide(&c, 1800000, false, false, false, true).deep_sleep);
    assert(!power_decide(&c, 1800000, false, true, false, false).deep_sleep);
    c.rgb_off_ms = 10000; c.backlight_off_ms = 60000; c.idle_timeout_ms = 0;
    d = power_decide(&c, 30000, false, false, false, false);
    assert(d.rgb_off && !d.backlight_off && !d.idle);
    c.rgb_off_ms = 0; c.backlight_off_ms = 0; c.deep_sleep_ms = 0;
    assert(power_config_valid(&c));
    d = power_decide(&c, 90000000ULL, false, false, false, false);
    assert(!d.rgb_off && !d.backlight_off && !d.idle && !d.deep_sleep);
    c.rgb_off_ms = 999; assert(!power_config_valid(&c));
    c.rgb_off_ms = 1001; assert(!power_config_valid(&c));
    c.rgb_off_ms = 86401000; assert(!power_config_valid(&c));
    c = (struct zmk_power_config)ZMK_POWER_DEFAULTS;
    c.deep_sleep_ms = 60000; assert(!power_config_valid(&c));
    puts("PASS: independent light/idle clocks, held keys, USB policy, lock gating, opt-in deep sleep, Studio protection, timer validation");
    return 0;
}
