/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
enum led_motion_mode { LED_MOTION_NORMAL, LED_MOTION_MIXED, LED_MOTION_ALL_LOCKED };
static inline enum led_motion_mode led_motion_select(unsigned connected, unsigned locked) {
    locked &= connected;
    if(!locked) return LED_MOTION_NORMAL;
    return locked==connected ? LED_MOTION_ALL_LOCKED : LED_MOTION_MIXED;
}
/* 2400ms smoothstep breath, scaled by the configured brightness afterwards.
 * ALL: 5..100%; MIXED: 65..100%, signalling another peer can still be usable. */
static inline unsigned led_motion_level(enum led_motion_mode mode, uint32_t now) {
    if(mode==LED_MOTION_NORMAL) return 100;
    uint32_t phase=now%2400u;
    uint32_t triangle=phase<1200u ? phase : 2400u-phase;
    uint32_t x=triangle*1000u/1200u;
    uint32_t smooth=x*x*(3000u-2u*x)/1000000u;
    return mode==LED_MOTION_ALL_LOCKED ? 5u+95u*smooth/1000u : 65u+35u*smooth/1000u;
}
