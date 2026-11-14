/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define LED_LINK_QUEUE 8
struct led_link_state {
    uint8_t mask, head, count;
    bool events[LED_LINK_QUEUE], active, connected;
    uint32_t started;
};
static inline void led_link_push(struct led_link_state *s, bool connected) {
    /* If transitions flap excessively, keep the newest bounded history. */
    if (s->count == LED_LINK_QUEUE) {
        s->head = (s->head + 1) % LED_LINK_QUEUE;
        s->count--;
    }
    s->events[(s->head + s->count++) % LED_LINK_QUEUE] = connected;
}
/* Poll connection state, then optionally override the baseline LED level.
 * enabled respects OFF/brightness=0; identify keeps its existing priority.
 * All timing is nonblocking and independent of the configured idle period. */
static inline bool led_link_update(struct led_link_state *s, uint32_t now,
                                   uint8_t mask, bool enabled, bool identify, bool *on) {
    uint8_t changed = (s->mask ^ mask) & 3;
    s->mask = mask;
    if (!enabled || identify) {
        s->active = false;
        s->count = s->head = 0;
        return false;
    }
    for (unsigned slot = 0; slot < 2; slot++) {
        if (changed & (1u << slot)) { led_link_push(s, mask & (1u << slot)); }
    }
    if (s->active) {
        uint32_t duration = s->connected ? 600 : 1100;
        if (now - s->started >= duration) { s->active = false; }
    }
    if (!s->active && s->count) {
        s->connected = s->events[s->head];
        s->head = (s->head + 1) % LED_LINK_QUEUE;
        s->count--;
        s->active = true;
        s->started = now;
    }
    if (!s->active) { return false; }
    uint32_t ms = now - s->started;
    /* Begin dark so both pulses are visible even over steady ON. */
    *on = s->connected ? ((ms >= 100 && ms < 200) || (ms >= 300 && ms < 400))
                       : ((ms >= 100 && ms < 400) || (ms >= 600 && ms < 900));
    return true;
}
