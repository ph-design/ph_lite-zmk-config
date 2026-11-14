/* SPDX-License-Identifier: MIT
 * Portable policy for EVO Motion Guard preview. Units: milliseconds and mg.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MG_KEYS 128
#define MG_QUEUE 128
#define MG_SETTLE_MS 60
#define MG_CARRIED_SETTLE_MS 180
#define MG_CARRY_HISTORY_MS 300
#define MG_STABLE_SPAN_MG 60
#define MG_VERIFY_MS 30
#define MG_STALE_MS 150

struct mg_config {
    uint32_t carried_settle_ms, brief_settle_ms, stable_span_mg, departure_mg, verify_ms;
};
#define MG_CONFIG_DEFAULTS {180, 60, 60, 200, 30}
static inline bool mg_config_valid(const struct mg_config *c) {
    return c->carried_settle_ms >= 100 && c->carried_settle_ms <= 600 &&
        c->brief_settle_ms >= 40 && c->brief_settle_ms <= 200 &&
        c->carried_settle_ms >= c->brief_settle_ms &&
        c->stable_span_mg >= 20 && c->stable_span_mg <= 100 &&
        c->departure_mg >= 120 && c->departure_mg <= 350 &&
        c->verify_ms >= 20 && c->verify_ms <= 60;
}
enum mg_state { MG_READY, MG_VERIFY, MG_CARRY, MG_SETTLING };
struct mg_key { uint32_t position; bool down; int64_t time; };
typedef void (*mg_emit_fn)(struct mg_key, void *);
struct mg_guard {
    struct mg_config config;
    enum mg_state state;
    bool enabled, valid, have_previous, flat;
    int32_t base[3], previous[3], candidate[3];
    int32_t window_min[3], window_max[3];
    int64_t last_sample, entered, quiet_since, carry_since;
    uint32_t settle_ms;
    bool physical[MG_KEYS], blocked[MG_KEYS], sent[MG_KEYS];
    struct mg_key queue[MG_QUEUE];
    unsigned queued;
    mg_emit_fn emit;
    void *context;
};
static inline int32_t mg_abs(int32_t a) { return a < 0 ? -a : a; }
static inline int32_t mg_max(int32_t a, int32_t b) { return a > b ? a : b; }
static inline bool mg_blocks(const struct mg_guard *g) {
    return g->enabled && (g->state == MG_CARRY || g->state == MG_SETTLING);
}
static inline void mg_emit(struct mg_guard *g, struct mg_key k) {
    if (k.position >= MG_KEYS) { return; }
    if (k.down == g->sent[k.position]) { return; }
    g->sent[k.position] = k.down;
    g->emit(k, g->context);
}
static inline void mg_lock(struct mg_guard *g, int64_t now) {
    if (g->carry_since < 0) { g->carry_since = now; }
    g->state = MG_CARRY;
    g->entered = now;
    g->quiet_since = -1;
    g->queued = 0;
    for (unsigned p = 0; p < MG_KEYS; p++) {
        g->blocked[p] = g->physical[p];
        if (g->sent[p]) { mg_emit(g, (struct mg_key){p, false, now}); }
    }
}
static inline void mg_ready(struct mg_guard *g) {
    g->state = MG_READY;
    g->carry_since = -1;
    /* Do not burst replay into the finite radio TX queue. */
}
static inline bool mg_drain(struct mg_guard *g, int64_t now) {
    if (mg_blocks(g) || !g->queued) { return false; }
    struct mg_key k = g->queue[0];
    memmove(g->queue, g->queue + 1, (--g->queued) * sizeof(g->queue[0]));
    k.time = now;
    mg_emit(g, k);
    return true;
}
static inline void mg_init(struct mg_guard *g, mg_emit_fn emit, void *context) {
    memset(g, 0, sizeof(*g));
    g->config = (struct mg_config)MG_CONFIG_DEFAULTS;
    g->state = MG_CARRY;
    g->enabled = true;
    g->quiet_since = -1;
    g->carry_since = -1;
    g->settle_ms = MG_SETTLE_MS;
    g->emit = emit;
    g->context = context;
}
static inline void mg_enable(struct mg_guard *g, bool enabled) {
    if (g->enabled == enabled) { return; }
    g->enabled = enabled;
    if (enabled) {
        g->queued = 0;
        g->valid = false;
        g->have_previous = false;
        mg_lock(g, g->last_sample);
    } else { mg_ready(g); }
}
static inline void mg_sample(struct mg_guard *g, int64_t now,
                             int32_t x, int32_t y, int32_t z) {
    int32_t a[3] = {x, y, z};
    int64_t norm2 = (int64_t)x*x + (int64_t)y*y + (int64_t)z*z;
    /* Orientation is a ratio, not an acceleration magnitude test. Vertical
     * typing impulses must not turn a desk-facing pose into a carry lock. */
    g->flat = z >= 500 && ((int64_t)x*x + (int64_t)y*y) * 100 <= (int64_t)z*z * 30;
    bool resting = g->flat && norm2 >= 850LL*850 && norm2 <= 1150LL*1150;
    int32_t step = 0, departure = 0;
    bool fresh = g->have_previous && now - g->last_sample <= MG_STALE_MS;
    for (unsigned i = 0; i < 3; i++) {
        if (fresh) { step = mg_max(step, mg_abs(a[i] - g->previous[i])); }
        departure = mg_max(departure, mg_abs(a[i] - g->base[i]));
        g->previous[i] = a[i];
    }
    g->last_sample = now;
    g->valid = true;
    g->have_previous = true;
    if (!g->enabled) { return; }
    if (!fresh && g->state != MG_CARRY) { mg_lock(g, now); }

    bool quiet = fresh && resting && step <= 65;
    if (quiet && mg_blocks(g) && g->quiet_since >= 0) {
        for (unsigned i = 0; i < 3; i++) {
            if (a[i] < g->window_min[i]) { g->window_min[i] = a[i]; }
            if (a[i] > g->window_max[i]) { g->window_max[i] = a[i]; }
            if (g->window_max[i] - g->window_min[i] > (int32_t)g->config.stable_span_mg) {
                quiet = false;
            }
        }
    }
    if (quiet) {
        if (g->quiet_since < 0) {
            g->quiet_since = now;
            for (unsigned i = 0; i < 3; i++) {
                g->window_min[i] = g->window_max[i] = a[i];
            }
        }
    } else {
        g->quiet_since = -1;
        if (mg_blocks(g) && g->carry_since < 0) { g->carry_since = now; }
    }

    /* A clear sideways/inverted pose is immediately locked. A short
     * acceleration spike, including a strong keystroke, is not enough. */
    bool sideways = z < -250 || (z < 800 && (mg_abs(x) >= 650 || mg_abs(y) >= 650));
    if (sideways) {
        mg_lock(g, now);
        return;
    }
    switch (g->state) {
    case MG_READY:
        if (!g->flat || departure >= (int32_t)g->config.departure_mg) {
            g->state = MG_VERIFY;
            g->entered = now;
            g->quiet_since = -1;
            for (unsigned i = 0; i < 3; i++) { g->candidate[i] = a[i] - g->base[i]; }
        } else if (quiet) {
            for (unsigned i = 0; i < 3; i++) {
                g->base[i] += (a[i] - g->base[i]) / 32;
            }
        }
        break;
    case MG_VERIFY:
        if (g->flat && departure < (int32_t)g->config.departure_mg) {
            mg_ready(g);
        } else {
            int64_t direction = 0;
            for (unsigned i = 0; i < 3; i++) {
                direction += (int64_t)g->candidate[i] * (a[i] - g->base[i]);
            }
            /* Alternating vibrations are not sustained departure. */
            if (direction <= 0) {
                g->entered = now;
                for (unsigned i = 0; i < 3; i++) { g->candidate[i] = a[i] - g->base[i]; }
            } else if (now - g->entered >= g->config.verify_ms) { mg_lock(g, now); }
        }
        break;
    case MG_CARRY:
        if (quiet) {
            g->state = MG_SETTLING;
            g->entered = now;
            g->settle_ms = g->carry_since >= 0 && now - g->carry_since >= MG_CARRY_HISTORY_MS
                               ? g->config.carried_settle_ms : g->config.brief_settle_ms;
        }
        break;
    case MG_SETTLING:
        if (!quiet) { mg_lock(g, now); }
        else if (now - g->quiet_since >= g->settle_ms) {
            for (unsigned i = 0; i < 3; i++) { g->base[i] = a[i]; }
            mg_ready(g);
        }
        break;
    }
}
static inline void mg_key(struct mg_guard *g, struct mg_key k) {
    if (k.position >= MG_KEYS) { return; }
    g->physical[k.position] = k.down;
    if (g->enabled && (!g->valid || k.time - g->last_sample > MG_STALE_MS)) {
        mg_lock(g, k.time);
    }
    if (g->blocked[k.position]) {
        if (!k.down) { g->blocked[k.position] = false; }
        return;
    }
    if (!mg_blocks(g) && !g->queued) { mg_emit(g, k); return; }
    if (g->state == MG_SETTLING || !mg_blocks(g)) {
        if (g->queued < MG_QUEUE) { g->queue[g->queued++] = k; }
        else { mg_lock(g, k.time); }
        return;
    }
    if (k.down) { g->blocked[k.position] = true; }
}
