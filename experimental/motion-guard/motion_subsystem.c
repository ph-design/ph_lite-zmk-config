/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/behavior.h>
#include <zmk/motion.h>
#include <zmk/motion_guard.h>
#include <zmk/power.h>
#include <zmk/tap.h>
#include "../guard.h"
#include <zmk/studio/rpc.h>
#include <zmk/events/motion_live_state_changed.h>

#if DT_HAS_CHOSEN(zmk_imu)

ZMK_RPC_SUBSYSTEM(motion)

#define MOTION_RESPONSE(type, ...) ZMK_RPC_RESPONSE(motion, type, __VA_ARGS__)
#define MOTION_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(motion, type, __VA_ARGS__)

// unset binding is a negative behavior_id in the proto
static void binding_to_proto(const struct zmk_behavior_binding *b,
                             zmk_keymap_BehaviorBinding *out) {
    if (b && b->behavior_dev) {
        out->behavior_id = zmk_behavior_get_local_id(b->behavior_dev);
        out->param1 = b->param1;
        out->param2 = b->param2;
    } else {
        out->behavior_id = -1;
        out->param1 = 0;
        out->param2 = 0;
    }
}

static bool binding_from_proto(const zmk_keymap_BehaviorBinding *in,
                               struct zmk_behavior_binding *out) {
    if (in->behavior_id <= 0) {
        *out = (struct zmk_behavior_binding){0};
        return true;
    }

    const char *name = zmk_behavior_find_behavior_name_from_local_id(in->behavior_id);
    if (!name) {
        return false;
    }

    *out = (struct zmk_behavior_binding){
        .behavior_dev = name,
        .local_id = in->behavior_id,
        .param1 = in->param1,
        .param2 = in->param2,
    };
    return true;
}

static void tap_to_proto(const struct zmk_motion_tap_config *cfg, zmk_motion_TapConfig *out) {
    out->enabled = cfg->enabled;
    out->threshold = cfg->threshold;
    out->time_limit_ms = cfg->time_limit_ms;
    out->latency_ms = cfg->latency_ms;
    out->window_ms = cfg->window_ms;
    out->layer_mask = cfg->layer_mask;
    out->has_left_single_binding = cfg->left_single.behavior_dev != NULL;
    out->has_left_double_binding = cfg->left_double.behavior_dev != NULL;
    out->has_right_single_binding = cfg->right_single.behavior_dev != NULL;
    out->has_right_double_binding = cfg->right_double.behavior_dev != NULL;
    binding_to_proto(&cfg->left_single, &out->left_single_binding);
    binding_to_proto(&cfg->left_double, &out->left_double_binding);
    binding_to_proto(&cfg->right_single, &out->right_single_binding);
    binding_to_proto(&cfg->right_double, &out->right_double_binding);
}

static bool tap_from_proto(const zmk_motion_TapConfig *in, struct zmk_motion_tap_config *out) {
    out->enabled = in->enabled;
    out->threshold = in->threshold;
    out->time_limit_ms = in->time_limit_ms;
    out->latency_ms = in->latency_ms;
    out->window_ms = in->window_ms;
    out->layer_mask = in->layer_mask;

    bool ok = true;
    ok &= binding_from_proto(&in->left_single_binding, &out->left_single);
    ok &= binding_from_proto(&in->left_double_binding, &out->left_double);
    ok &= binding_from_proto(&in->right_single_binding, &out->right_single);
    ok &= binding_from_proto(&in->right_double_binding, &out->right_double);
    return ok;
}

zmk_studio_Response get_capabilities(const zmk_studio_Request *req) {
    zmk_motion_Capabilities resp = zmk_motion_Capabilities_init_zero;

    strncpy(resp.sensor, zmk_motion_sensor_name(), sizeof(resp.sensor) - 1);
    bool ready = zmk_motion_available();
    resp.supports_tap = ready;
    resp.supports_double_tap = ready;
    resp.supports_carry = ready;
    resp.supports_still_wake = ready;
    resp.supports_sleep_wake = ready;
    resp.threshold_max = 127;
    resp.guard_version = ready ? 1 : 0;
    resp.power_version = 1;
    resp.tap_version = ready ? 1 : 0;
    resp.power_lights = (IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW) ? 1 : 0) | (IS_ENABLED(CONFIG_ZMK_BACKLIGHT) ? 2 : 0);
    resp.supports_still_wake = false;
    resp.supports_sleep_wake = false;

    return MOTION_RESPONSE(capabilities, resp);
}

zmk_studio_Response get_tap_config(const zmk_studio_Request *req) {
    struct zmk_motion_tap_config cfg;
    zmk_motion_TapConfig resp = zmk_motion_TapConfig_init_zero;

    zmk_motion_get_tap_config(&cfg);
    tap_to_proto(&cfg, &resp);
    struct zmk_tap_options o; zmk_tap_get_options(&o);
    resp.axes_mask = o.axes; resp.split_sides = o.split; resp.typing_quiet_ms = o.quiet_ms;

    return MOTION_RESPONSE(tap_config, resp);
}

zmk_studio_Response set_tap_config(const zmk_studio_Request *req) {
    struct zmk_motion_tap_config cfg = {0};

    if (!tap_from_proto(&req->subsystem.motion.request_type.set_tap_config, &cfg)) {
        LOG_WRN("tap config decode rejected");
        return MOTION_RESPONSE(set_tap_config, false);
    }

    const zmk_motion_TapConfig *in = &req->subsystem.motion.request_type.set_tap_config;
    struct zmk_tap_options previous; zmk_tap_get_options(&previous);
    struct zmk_tap_options o = {in->axes_mask, in->typing_quiet_ms, in->split_sides};
    bool extended = in->axes_mask || in->typing_quiet_ms || in->split_sides;
    if (extended && !zmk_tap_options_valid(&o)) return MOTION_RESPONSE(set_tap_config, false);
    int rc = extended ? zmk_tap_set_options(&o) : 0;
    if (!rc) rc = zmk_motion_set_tap_config(&cfg);
    if (rc && extended) zmk_tap_set_options(&previous);
    return MOTION_RESPONSE(set_tap_config, rc == 0);
}

zmk_studio_Response get_carry_config(const zmk_studio_Request *req) {
    struct zmk_motion_carry_config cfg;
    zmk_motion_CarryConfig resp = zmk_motion_CarryConfig_init_zero;

    zmk_motion_get_carry_config(&cfg);
    resp.enabled = cfg.enabled;
    resp.motion_threshold = cfg.motion_threshold;
    resp.motion_duration_ms = cfg.motion_duration_ms;

    struct mg_config guard;
    zmk_motion_guard_get_config(&guard);
    resp.carried_settle_ms = guard.carried_settle_ms;
    resp.brief_settle_ms = guard.brief_settle_ms;
    resp.stable_span_mg = guard.stable_span_mg;
    resp.departure_mg = guard.departure_mg;
    resp.verify_ms = guard.verify_ms;
    return MOTION_RESPONSE(carry_config, resp);
}

zmk_studio_Response set_carry_config(const zmk_studio_Request *req) {
    struct zmk_motion_carry_config cfg = {
        .enabled = req->subsystem.motion.request_type.set_carry_config.enabled,
        .motion_threshold = req->subsystem.motion.request_type.set_carry_config.motion_threshold,
        .motion_duration_ms =
            req->subsystem.motion.request_type.set_carry_config.motion_duration_ms,
    };

    const zmk_motion_CarryConfig *in = &req->subsystem.motion.request_type.set_carry_config;
    struct mg_config guard = {in->carried_settle_ms, in->brief_settle_ms, in->stable_span_mg,
        in->departure_mg, in->verify_ms};
    bool extended = guard.carried_settle_ms || guard.brief_settle_ms || guard.stable_span_mg ||
        guard.departure_mg || guard.verify_ms;
    if (extended && !mg_config_valid(&guard)) { return MOTION_RESPONSE(set_carry_config, false); }
    int rc = zmk_motion_set_carry_config(&cfg);
    if (!rc && extended) { rc = zmk_motion_guard_set_config(&guard); }
    return MOTION_RESPONSE(set_carry_config, rc == 0);
}

zmk_studio_Response get_still_wake_config(const zmk_studio_Request *req) {
    struct zmk_motion_still_wake_config cfg;
    zmk_motion_StillWakeConfig resp = zmk_motion_StillWakeConfig_init_zero;

    zmk_motion_get_still_wake_config(&cfg);
    resp.enabled = cfg.enabled;
    resp.settle_duration_ms = cfg.settle_duration_ms;

    return MOTION_RESPONSE(still_wake_config, resp);
}

zmk_studio_Response set_still_wake_config(const zmk_studio_Request *req) {
    struct zmk_motion_still_wake_config cfg = {
        .enabled = req->subsystem.motion.request_type.set_still_wake_config.enabled,
        .settle_duration_ms =
            req->subsystem.motion.request_type.set_still_wake_config.settle_duration_ms,
    };

    return MOTION_RESPONSE(set_still_wake_config, zmk_motion_set_still_wake_config(&cfg) == 0);
}

zmk_studio_Response get_sleep_wake_config(const zmk_studio_Request *req) {
    struct zmk_motion_sleep_wake_config cfg;
    zmk_motion_SleepWakeConfig resp = zmk_motion_SleepWakeConfig_init_zero;

    zmk_motion_get_sleep_wake_config(&cfg);
    resp.enabled = cfg.enabled;
    resp.threshold = cfg.threshold;
    resp.duration_ms = cfg.duration_ms;

    return MOTION_RESPONSE(sleep_wake_config, resp);
}

zmk_studio_Response set_sleep_wake_config(const zmk_studio_Request *req) {
    struct zmk_motion_sleep_wake_config cfg = {
        .enabled = req->subsystem.motion.request_type.set_sleep_wake_config.enabled,
        .threshold = req->subsystem.motion.request_type.set_sleep_wake_config.threshold,
        .duration_ms = req->subsystem.motion.request_type.set_sleep_wake_config.duration_ms,
    };

    return MOTION_RESPONSE(set_sleep_wake_config, zmk_motion_set_sleep_wake_config(&cfg) == 0);
}

zmk_studio_Response get_tap_diagnostics(const zmk_studio_Request *req) {
    struct zmk_tap_diagnostics d; zmk_tap_get_diagnostics(&d);
    zmk_motion_TapDiagnostics out = {.irq_count = d.irq, .single_count = d.single, .double_count = d.dbl,
        .gesture_count = d.gestures, .action_count = d.actions, .suppressed_count = d.suppressed,
        .queue_drops = d.drops, .odr_hz = d.odr, .engine = d.engine, .test_remaining_ms = d.test_ms,
        .last_source = d.src, .last_gesture = d.gesture, .x_count = d.axes[0], .y_count = d.axes[1],
        .z_count = d.axes[2], .full_scale_mg = d.full_scale_mg, .error = d.error,
        .x_mg = d.current[0], .y_mg = d.current[1], .z_mg = d.current[2]};
    out.registers.size = 11; memcpy(out.registers.bytes, d.registers, 11);
    return MOTION_RESPONSE(tap_diagnostics, out);
}
zmk_studio_Response set_tap_test(const zmk_studio_Request *req) {
    return MOTION_RESPONSE(set_tap_test, zmk_tap_set_test(req->subsystem.motion.request_type.set_tap_test) == 0);
}
zmk_studio_Response get_tap_capture(const zmk_studio_Request *req) {
    struct zmk_tap_capture c; zmk_tap_get_capture(&c);
    zmk_motion_TapCapture out = {.sequence = c.sequence, .count = c.count};
    out.frames.size = c.count * 8; memcpy(out.frames.bytes, c.frames, out.frames.size);
    return MOTION_RESPONSE(tap_capture, out);
}

zmk_studio_Response get_power_config(const zmk_studio_Request *req) {
    struct zmk_power_config c; zmk_power_get_config(&c);
    zmk_motion_PowerConfig out = {.idle_timeout_ms = c.idle_timeout_ms, .rgb_off_ms = c.rgb_off_ms,
        .backlight_off_ms = c.backlight_off_ms, .deep_sleep_ms = c.deep_sleep_ms,
        .on_usb = c.on_usb, .off_when_locked = c.off_when_locked};
    return MOTION_RESPONSE(power_config, out);
}
zmk_studio_Response set_power_config(const zmk_studio_Request *req) {
    const zmk_motion_PowerConfig *in = &req->subsystem.motion.request_type.set_power_config;
    struct zmk_power_config c = {in->idle_timeout_ms, in->rgb_off_ms, in->backlight_off_ms,
        in->deep_sleep_ms, in->on_usb, in->off_when_locked};
    return MOTION_RESPONSE(set_power_config, zmk_power_set_config(&c) == 0);
}

zmk_studio_Response save_state(const zmk_studio_Request *req) {
    return MOTION_RESPONSE(save_state, zmk_motion_save_state() == 0 && zmk_motion_guard_save_config() == 0 && zmk_power_save_config() == 0 && zmk_tap_save_options() == 0);
}

zmk_studio_Response set_live_stream(const zmk_studio_Request *req) {
    bool on = req->subsystem.motion.request_type.set_live_stream;

    zmk_motion_set_live_stream(on);
    return MOTION_RESPONSE(set_live_stream, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_tap_diagnostics, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_tap_test, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_tap_capture, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_power_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_power_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_capabilities, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_tap_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_tap_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_carry_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_carry_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_still_wake_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_still_wake_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, get_sleep_wake_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_sleep_wake_config, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, save_state, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(motion, set_live_stream, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int motion_reset_all(void) {
    int rc = zmk_motion_settings_reset();
    int guard_rc = zmk_motion_guard_reset_config();
    int power_rc = zmk_power_reset_config();
    int tap_rc = zmk_tap_reset();
    return rc ? rc : guard_rc ? guard_rc : power_rc ? power_rc : tap_rc;
}
ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(motion, motion_reset_all);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) {
    const struct zmk_motion_live_state_changed *ev = as_zmk_motion_live_state_changed(eh);
    if (!ev) {
        return -ENOTSUP;
    }

    *n = MOTION_NOTIFICATION(live_state, ((zmk_motion_LiveState){
                                             .magnitude = ev->state.magnitude,
                                             .orientation = ev->state.orientation,
                                             .carry_active = ev->state.carry_active,
                                             .tap_detected = ev->state.tap_detected,
                                             .last_click_src = ev->state.last_click_src,
                                         }));
    return 0;
}

ZMK_RPC_EVENT_MAPPER(motion, event_mapper, zmk_motion_live_state_changed);

#endif /* DT_HAS_CHOSEN(zmk_imu) */
