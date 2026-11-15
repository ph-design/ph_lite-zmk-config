/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
enum tap_gesture { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };
struct tap_policy { bool pending; unsigned side; int64_t first, last; bool have_last; };
static inline void tap_cancel(struct tap_policy *p) { p->pending = false; }
static inline enum tap_gesture tap_expire(struct tap_policy *p, int64_t now,
    unsigned gap, bool single) {
    if (!p->pending || now - p->first <= gap) return TAP_NONE;
    p->pending = false;
    return single ? TAP_SINGLE : TAP_NONE;
}
static inline enum tap_gesture tap_pulse(struct tap_policy *p, int64_t now, unsigned side,
    bool hardware_double, bool single, bool dbl, unsigned latency, unsigned window) {
    if (p->have_last && now - p->last < latency) return TAP_NONE;
    p->last = now; p->have_last = true;
    if (hardware_double) { p->pending = false; p->side = side; return TAP_DOUBLE; }
    if (!dbl) { p->pending = false; p->side = side; return TAP_SINGLE; }
    if (p->pending && p->side == side && now - p->first <= latency + window) {
        p->pending = false; return TAP_DOUBLE;
    }
    p->pending = true; p->side = side; p->first = now;
    (void)single;
    return TAP_NONE;
}
