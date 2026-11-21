/* SPDX-License-Identifier: MIT */

#define DT_DRV_COMPAT phdesign_ot60_haptics

#include <drivers/behavior.h>
#include <dt-bindings/zmk/ot60-haptics.h>
#include <zephyr/device.h>
#include <zephyr/drivers/haptics.h>
#include <zephyr/drivers/haptics/drv2605.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum ot60_effect {
    OT60_EFFECT_MANUAL,
    OT60_EFFECT_KEY,
    OT60_EFFECT_LAYER,
};

enum {
    OT60_OVERDRIVE_MS = 4,
    OT60_SUSTAIN_MS = 8,
    OT60_BRAKE_MS = 4,
    OT60_DRIVER_WAKE_MS = 2,
    OT60_KEY_EVENT_MAX_AGE_MS = 100,
    OT60_ACTUATOR_RETRY_MS = 1000,
    OT60_FALLBACK_ROM_PEAK_MV = 1000,
    OT60_FALLBACK_KEY_PEAK_MV = 1400,
};

struct haptic_request {
    int64_t queued_at;
    uint8_t targets;
    enum ot60_effect effect;
};

struct haptic_actuator {
    const struct device *dev;
    uint8_t target;
    bool playable;
    bool using_fallback;
    uint16_t rated_mv;
    uint16_t peak_mv;
    int64_t retry_after;
};

static struct haptic_actuator actuators[] = {
    {.dev = DEVICE_DT_GET(DT_NODELABEL(ot60_lra_left)),
     .target = OT60_LRA_LEFT,
     .rated_mv = DT_PROP(DT_NODELABEL(ot60_lra_left), vib_rated_mv),
     .peak_mv = DT_PROP(DT_NODELABEL(ot60_lra_left), vib_overdrive_mv)},
    {.dev = DEVICE_DT_GET(DT_NODELABEL(ot60_lra_right)),
     .target = OT60_LRA_RIGHT,
     .rated_mv = DT_PROP(DT_NODELABEL(ot60_lra_right), vib_rated_mv),
     .peak_mv = DT_PROP(DT_NODELABEL(ot60_lra_right), vib_overdrive_mv)},
};

static struct drv2605_rom_data rom = {
    .trigger = DRV2605_MODE_INTERNAL_TRIGGER,
    .library = DRV2605_LIBRARY_LRA,
};

static const union drv2605_config_data rom_config = {.rom_data = &rom};

/* The OT60 worker writes its own timed samples, so the driver needs only the format. */
static struct drv2605_rtp_data key_rtp = {.format = DRV2605_RTP_SIGNED};
static const union drv2605_config_data key_rtp_config = {.rtp_data = &key_rtp};

static atomic_t haptics_ready;

K_MSGQ_DEFINE(ot60_haptic_requests, sizeof(struct haptic_request), 12,
              __alignof__(struct haptic_request));

static void queue_haptic(enum ot60_effect effect, uint8_t targets) {
    if (!atomic_get(&haptics_ready)) {
        return;
    }

    struct haptic_request request = {
        .queued_at = k_uptime_get(),
        .targets = targets,
        .effect = effect,
    };

    /* A layer cue can replace the oldest queued request when typing fills the queue. */
    if (k_msgq_put(&ot60_haptic_requests, &request, K_NO_WAIT) < 0 &&
        effect == OT60_EFFECT_LAYER) {
        struct haptic_request dropped;
        k_msgq_get(&ot60_haptic_requests, &dropped, K_NO_WAIT);
        k_msgq_put(&ot60_haptic_requests, &request, K_NO_WAIT);
    }
}

static void actuator_failed(struct haptic_actuator *actuator, int error) {
    actuator->retry_after = k_uptime_get() + OT60_ACTUATOR_RETRY_MS;
    LOG_WRN("OT60 actuator %u returned %d", actuator->target, error);
}

static void initialize_actuator(struct haptic_actuator *actuator) {
    const struct device *dev = actuator->dev;
    int ret = device_is_ready(dev) ? drv2605_hw_setup(dev) : -ENODEV;
    if (!ret) {
        ret = drv2605_calibrate_and_wait(dev);
        if (ret == -ERANGE) {
            LOG_WRN("OT60 actuator %u did not converge; using reduced open-loop drive",
                    actuator->target);
            uint16_t fallback_mv = MIN(actuator->peak_mv, OT60_FALLBACK_ROM_PEAK_MV);
            ret = drv2605_enable_lra_fallback(dev, fallback_mv);
            actuator->using_fallback = ret == 0;
        }
        if (!ret) {
            ret = drv2605_haptic_config(dev, DRV2605_HAPTICS_SOURCE_ROM, &rom_config);
        }
        int standby_ret = drv2605_set_standby(dev, true);
        if (!ret) {
            ret = standby_ret;
        }
    }

    if (ret < 0) {
        LOG_WRN("OT60 actuator %u failed boot calibration: %d", actuator->target, ret);
        return;
    }

    actuator->playable = true;
}

static void idle_actuator(struct haptic_actuator *actuator, bool stop) {
    int stop_ret = stop ? haptics_stop_output(actuator->dev) : 0;
    int idle_ret = drv2605_set_standby(actuator->dev, true);
    if (stop_ret < 0 || idle_ret < 0) {
        actuator_failed(actuator, stop_ret < 0 ? stop_ret : idle_ret);
    }
}

static void play_key_pulse(uint8_t awake) {
    static const uint8_t phase_ms[] = {OT60_OVERDRIVE_MS, OT60_SUSTAIN_MS, OT60_BRAKE_MS};
    int8_t samples[ARRAY_SIZE(actuators)][ARRAY_SIZE(phase_ms)];
    int64_t phase_end[ARRAY_SIZE(actuators)] = {0};
    uint8_t prepared = 0;

    /* Set both drivers to zero input before starting either pulse. */
    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        struct haptic_actuator *actuator = &actuators[i];
        if (!(awake & actuator->target)) {
            continue;
        }

        uint16_t peak_mv = actuator->using_fallback
                               ? MIN(actuator->peak_mv, OT60_FALLBACK_KEY_PEAK_MV)
                               : actuator->peak_mv;
        int ret = drv2605_haptic_config(actuator->dev, DRV2605_HAPTICS_SOURCE_RTP, &key_rtp_config);
        if (!ret) {
            ret = drv2605_set_lra_drive(actuator->dev, true, peak_mv, true);
        }
        if (ret < 0) {
            actuator_failed(actuator, ret);
        } else {
            /* Full kick, one cycle at a peak no greater than the RMS rating, then brake. */
            samples[i][0] = INT8_MAX;
            samples[i][1] = INT8_MAX * MIN(actuator->rated_mv, peak_mv) / peak_mv;
            samples[i][2] = -(INT8_MAX / 4);
            prepared |= actuator->target;
        }
    }

    uint8_t playing = prepared;
    for (size_t phase = 0; phase < ARRAY_SIZE(phase_ms); phase++) {
        for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
            struct haptic_actuator *actuator = &actuators[i];
            if (!(playing & actuator->target)) {
                continue;
            }
            if (phase_end[i]) {
                k_sleep(K_TIMEOUT_ABS_TICKS(phase_end[i]));
            }

            int ret = drv2605_rtp_set_input(actuator->dev, (uint8_t)samples[i][phase]);
            if (ret < 0) {
                actuator_failed(actuator, ret);
                playing &= ~actuator->target;
                if (!drv2605_rtp_finish(actuator->dev)) {
                    awake &= ~actuator->target;
                    prepared &= ~actuator->target;
                }
            } else {
                phase_end[i] = k_uptime_ticks() + k_ms_to_ticks_ceil64(phase_ms[phase]);
            }
        }
    }

    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        struct haptic_actuator *actuator = &actuators[i];
        if (!(prepared & actuator->target)) {
            continue;
        }
        if (playing & actuator->target) {
            k_sleep(K_TIMEOUT_ABS_TICKS(phase_end[i]));
        }
        int ret = drv2605_rtp_finish(actuator->dev);
        if (ret < 0) {
            actuator_failed(actuator, ret);
        } else {
            awake &= ~actuator->target;
        }
    }

    /* Defer recovery writes until both actuators have had their timed stop. */
    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        if (awake & actuators[i].target) {
            idle_actuator(&actuators[i], true);
        }
    }
}

static void play_rom_haptic(enum ot60_effect effect, uint8_t awake) {
    const bool layer_change = effect == OT60_EFFECT_LAYER;
    /* Short Double Sharp Tick 60% for layers, Strong Click 30% for manual playback. */
    const uint8_t waveform = layer_change ? 36 : 3;
    uint8_t busy = 0;
    uint8_t must_stop = 0;

    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        struct haptic_actuator *actuator = &actuators[i];
        if (!(awake & actuator->target)) {
            continue;
        }

        uint16_t peak_mv = actuator->using_fallback
                               ? MIN(actuator->peak_mv, OT60_FALLBACK_ROM_PEAK_MV)
                               : actuator->peak_mv;
        int ret = drv2605_set_lra_drive(actuator->dev, actuator->using_fallback, peak_mv, false);
        if (!ret) {
            ret = drv2605_play_rom_effect(actuator->dev, waveform);
        }
        if (ret < 0) {
            must_stop |= actuator->target;
            actuator_failed(actuator, ret);
        } else {
            busy |= actuator->target;
        }
    }

    int64_t deadline = k_uptime_get() + (layer_change ? 220 : 100);
    while (busy && k_uptime_get() < deadline) {
        k_msleep(4);
        for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
            struct haptic_actuator *actuator = &actuators[i];
            if (!(busy & actuator->target)) {
                continue;
            }

            int ret = drv2605_go_active(actuator->dev);
            if (ret <= 0) {
                busy &= ~actuator->target;
            }
            if (ret < 0) {
                must_stop |= actuator->target;
                actuator_failed(actuator, ret);
            }
        }
    }

    must_stop |= busy;
    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        struct haptic_actuator *actuator = &actuators[i];
        if (awake & actuator->target) {
            idle_actuator(actuator, must_stop & actuator->target);
        }
    }
}

static void play_haptic(const struct haptic_request *request) {
    uint8_t awake = 0;

    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        struct haptic_actuator *actuator = &actuators[i];
        if (!(request->targets & actuator->target) || !actuator->playable ||
            k_uptime_get() < actuator->retry_after) {
            continue;
        }

        int ret = drv2605_set_standby(actuator->dev, false);
        if (ret < 0) {
            actuator_failed(actuator, ret);
        } else {
            awake |= actuator->target;
        }
    }

    if (!awake) {
        return;
    }

    /* Let the last actuator leave standby before starting playback. */
    k_msleep(OT60_DRIVER_WAKE_MS);
    if (request->effect == OT60_EFFECT_KEY) {
        play_key_pulse(awake);
    } else {
        play_rom_haptic(request->effect, awake);
    }
}

static void haptics_thread(void *unused1, void *unused2, void *unused3) {
    /* Calibrate one motor at a time without blocking keyboard input or USB setup. */
    for (size_t i = 0; i < ARRAY_SIZE(actuators); i++) {
        initialize_actuator(&actuators[i]);
    }
    atomic_set(&haptics_ready, 1);

    for (;;) {
        struct haptic_request request;
        if (k_msgq_get(&ot60_haptic_requests, &request, K_FOREVER) < 0) {
            continue;
        }

        if (request.effect != OT60_EFFECT_KEY ||
            k_uptime_get() - request.queued_at <= OT60_KEY_EVENT_MAX_AGE_MS) {
            play_haptic(&request);
        }
    }
}

K_THREAD_DEFINE(ot60_haptics_worker, 1024, haptics_thread, NULL, NULL, NULL, 10, 0, 0);

static struct k_spinlock position_lock;
static int64_t last_position_stamp[ZMK_KEYMAP_LEN];
static atomic_t active_layer = ATOMIC_INIT(ZMK_KEYMAP_LAYER_ID_INVAL);

static zmk_keymap_layer_id_t active_layer_id(void) {
    return zmk_keymap_layer_index_to_id(zmk_keymap_highest_layer_active());
}

static int seed_active_layer(void) {
    atomic_set(&active_layer, active_layer_id());
    return 0;
}

SYS_INIT(seed_active_layer, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

static int haptics_listener(const zmk_event_t *event) {
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(event);
    if (position && position->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL &&
        position->position < ARRAY_SIZE(last_position_stamp)) {
        /* Combos can replay a position transition with the original timestamp. */
        const int64_t stamp = position->timestamp + 1;
        k_spinlock_key_t lock = k_spin_lock(&position_lock);
        bool fresh = stamp > last_position_stamp[position->position];
        if (fresh) {
            last_position_stamp[position->position] = stamp;
        }
        k_spin_unlock(&position_lock, lock);

        if (fresh) {
            queue_haptic(OT60_EFFECT_KEY, OT60_LRA_BOTH);
        }
    } else if (as_zmk_layer_state_changed(event)) {
        zmk_keymap_layer_id_t layer = active_layer_id();
        atomic_val_t previous = atomic_set(&active_layer, layer);
        if (previous != ZMK_KEYMAP_LAYER_ID_INVAL && previous != layer) {
            queue_haptic(OT60_EFFECT_LAYER, OT60_LRA_BOTH);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(ot60_haptics, haptics_listener);
ZMK_SUBSCRIPTION(ot60_haptics, zmk_position_state_changed);
ZMK_SUBSCRIPTION(ot60_haptics, zmk_layer_state_changed);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_pressed(struct zmk_behavior_binding *binding, struct zmk_behavior_binding_event event) {
    if (!binding->param1 || (binding->param1 & ~OT60_LRA_BOTH)) {
        return -EINVAL;
    }

    queue_haptic(OT60_EFFECT_MANUAL, binding->param1);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata target_values[] = {
    {.display_name = "Left", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = OT60_LRA_LEFT},
    {.display_name = "Right", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = OT60_LRA_RIGHT},
    {.display_name = "Both", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = OT60_LRA_BOTH},
};

static const struct behavior_parameter_metadata_set target_set = {
    .param1_values = target_values,
    .param1_values_len = ARRAY_SIZE(target_values),
};

static int get_parameter_metadata(const struct device *dev,
                                  struct behavior_parameter_metadata *metadata) {
    metadata->sets = &target_set;
    metadata->sets_len = 1;
    return 0;
}
#endif

static const struct behavior_driver_api ot60_haptics_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = get_parameter_metadata,
#endif
};

#define OT60_HAPTICS_DEFINE(inst)                                                                  \
    BEHAVIOR_DT_INST_DEFINE(inst, NULL, NULL, NULL, NULL, POST_KERNEL,                               \
                           CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ot60_haptics_api);

DT_INST_FOREACH_STATUS_OKAY(OT60_HAPTICS_DEFINE)

#endif
