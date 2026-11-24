/* Copyright (c) 2026 Team PHDesign
 * SPDX-License-Identifier: MIT
 */
#define DT_DRV_COMPAT phdesign_behavior_ot60_haptic

#include <zephyr/device.h>
#include <zephyr/drivers/haptics.h>
#include <zephyr/drivers/haptics/drv2605.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/atomic.h>
#include <drivers/behavior.h>
#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_REGISTER(ot60_haptics, CONFIG_ZMK_LOG_LEVEL);

static const struct device *const motor = DEVICE_DT_GET(DT_NODELABEL(right_haptic));
static const struct i2c_dt_spec motor_bus = I2C_DT_SPEC_GET(DT_NODELABEL(right_haptic));
static struct k_work_q motor_queue;
K_THREAD_STACK_DEFINE(motor_stack, 2048);
static atomic_t queue_started;
static atomic_t ready;
static atomic_t enabled = ATOMIC_INIT(1);
static atomic_t sleeping;
static atomic_t calibrate_pending = ATOMIC_INIT(1);
static atomic_t automatic_pending;
static atomic_t manual_effect;
static atomic_t manual_busy;
static uint8_t last_status;

#define HAPTIC_EFFECT_MIN 1
#define HAPTIC_EFFECT_MAX 123
#define HAPTIC_PROGRAMMATIC_STOP_EFFECT 118

/* All I2C and calibration waits run on a private queue, never in the key event handler. */
static int wait_for_go(uint32_t timeout_ms, bool preemptible) {
    const int64_t start = k_uptime_get();
    const int64_t deadline = start + timeout_ms;
    bool seen_go = false;
    int last_error = 0;
    do {
        uint8_t go;
        if (atomic_get(&sleeping)) {
            return -ECANCELED;
        }
        if (preemptible && (atomic_get(&manual_effect) || atomic_get(&calibrate_pending))) {
            return -EINTR;
        }
        int rc = i2c_reg_read_byte_dt(&motor_bus, 0x0c, &go);
        if (rc < 0) {
            /* The reference bench retries transient NACKs while a process is starting. */
            last_error = rc;
            k_msleep(2);
            continue;
        }
        last_error = 0;
        if (go & BIT(0)) {
            seen_go = true;
        } else if (seen_go || k_uptime_get() - start >= 8) {
            /* An initial zero is not completion: give the trigger a start window.
             * Very short effects may finish between samples, so do not require
             * seeing GO=1 forever when the device is otherwise responsive.
             */
            return 0;
        }
        k_msleep(2);
    } while (k_uptime_get() < deadline);
    return last_error < 0 ? last_error : -ETIMEDOUT;
}

static int check_status(void) {
    uint8_t status;
    int rc = drv2605_read_status(motor, &status);
    if (rc < 0) {
        return rc;
    }
    last_status = status;
    /* DIAG_RESULT, overtemperature, overcurrent. Never play after a failed calibration. */
    return (status & (BIT(3) | BIT(1) | BIT(0))) ? -EIO : 0;
}

static void log_motor_state(const char *stage, int result) {
#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
    uint8_t mode = 0xff, library = 0xff, sequence = 0xff, go = 0xff;
    uint8_t compensation = 0xff, bemf = 0xff, feedback = 0xff;
    /* Do not re-read STATUS here: its fault flags may be clear-on-read. */
    i2c_reg_read_byte_dt(&motor_bus, 0x01, &mode);
    i2c_reg_read_byte_dt(&motor_bus, 0x03, &library);
    i2c_reg_read_byte_dt(&motor_bus, 0x04, &sequence);
    i2c_reg_read_byte_dt(&motor_bus, 0x0c, &go);
    i2c_reg_read_byte_dt(&motor_bus, 0x18, &compensation);
    i2c_reg_read_byte_dt(&motor_bus, 0x19, &bemf);
    i2c_reg_read_byte_dt(&motor_bus, 0x1a, &feedback);
    LOG_INF("%s rc=%d ready=%d status=0x%02x mode=0x%02x lib=0x%02x seq=%u go=%u cal=%02x/%02x fb=%02x",
            stage, result, (int)atomic_get(&ready), last_status, mode, library,
            sequence, go, compensation, bemf, feedback);
#endif
}

static int configure_effect(uint32_t effect) {
    if (effect < HAPTIC_EFFECT_MIN || effect > HAPTIC_EFFECT_MAX) {
        return -EINVAL;
    }
    struct drv2605_rom_data rom = {
        .trigger = DRV2605_MODE_INTERNAL_TRIGGER,
        .library = DRV2605_LIBRARY_LRA,
        .seq_regs = {(uint8_t)effect, 0, 0, 0, 0, 0, 0, 0},
    };
    union drv2605_config_data config = {.rom_data = &rom};
    return drv2605_haptic_config(motor, DRV2605_HAPTICS_SOURCE_ROM, &config);
}

static void motor_work_cb(struct k_work *work) {
    if (atomic_get(&sleeping)) {
        return;
    }
    int rc = pm_device_action_run(motor, PM_DEVICE_ACTION_RESUME);
    if (rc < 0 && rc != -EALREADY) {
        goto failed;
    }
    rc = 0;

    if (atomic_cas(&calibrate_pending, 1, 0)) {
        atomic_clear(&ready);
        rc = drv2605_hw_setup(motor);
        if (rc == 0 && !atomic_get(&sleeping)) {
            rc = drv2605_calibrate(motor);
        } else if (rc == 0) {
            rc = -ECANCELED;
        }
        if (rc == 0) {
            rc = wait_for_go(2500, false);
        }
        if (rc == 0) {
            rc = check_status();
        }
        if (rc == 0) {
            rc = configure_effect(1);
        }
        if (rc == 0) {
            atomic_set(&ready, 1);
            LOG_INF("Right LRA calibrated: 125 Hz, rated 1.0 V, clamp 1.8 V");
        } else if (rc == -ECANCELED) {
            atomic_set(&calibrate_pending, 1);
        }
        log_motor_state("calibration", rc);
    } else if (atomic_get(&ready)) {
        uint32_t effect = atomic_set(&manual_effect, 0);
        bool manual = effect != 0;
        if (manual) {
            atomic_clear(&automatic_pending);
        } else if (atomic_set(&automatic_pending, 0) && atomic_get(&enabled)) {
            effect = 1;
        }
        if (effect) {
            /* Always rewrite all eight slots so manual playback cannot change
             * subsequent ordinary-key feedback or leave a stale ROM sequence.
             */
            rc = configure_effect(effect);
            if (manual) {
                LOG_INF("Manual ROM effect %u", effect);
            }
            if (rc == 0) {
                rc = haptics_start_output(motor);
            }
            if (rc == 0) {
                uint32_t timeout = !manual ? 500 :
                    (effect == HAPTIC_PROGRAMMATIC_STOP_EFFECT ? 1000 : 5000);
                rc = wait_for_go(timeout, true);
                /* Effect 118 is explicitly intended for host-controlled stopping. */
                if (manual && effect == HAPTIC_PROGRAMMATIC_STOP_EFFECT && rc == -ETIMEDOUT) {
                    rc = 0;
                }
            }
            if (rc == 0) {
                rc = check_status();
            }
            if (manual || rc < 0) {
                log_motor_state("playback", rc);
            }
        }
    }

    /* Clear playback and enter standby because U7 EN is physically tied to VDD. */
    int stop_rc = i2c_reg_write_byte_dt(&motor_bus, 0x0c, 0);
    int standby_rc = pm_device_action_run(motor, PM_DEVICE_ACTION_SUSPEND);
    if (rc == 0 && stop_rc < 0) {
        rc = stop_rc;
    }
    if (rc == 0 && standby_rc < 0 && standby_rc != -EALREADY) {
        rc = standby_rc;
    }
failed:
    if (!atomic_get(&manual_effect)) {
        atomic_clear(&manual_busy);
    }
    if (rc < 0 && rc != -ECANCELED && rc != -EINTR) {
        atomic_clear(&ready);
        LOG_ERR("Right haptic disabled after error %d; Fn+C retries calibration", rc);
    }
}
K_WORK_DEFINE(motor_work, motor_work_cb);

static int request_effect(uint32_t effect) {
    if (effect < HAPTIC_EFFECT_MIN || effect > HAPTIC_EFFECT_MAX) {
        return -EINVAL;
    }
    if (!atomic_get(&queue_started) || atomic_get(&sleeping)) {
        return -EBUSY;
    }
    if (!atomic_get(&ready)) {
        LOG_WRN("Effect %u rejected: calibration is not ready, status=0x%02x", effect, last_status);
        return -EAGAIN;
    }
    atomic_set(&manual_busy, 1);
    atomic_clear(&automatic_pending);
    atomic_set(&manual_effect, effect);
    k_work_submit_to_queue(&motor_queue, &motor_work);
    return 0;
}

static int ot60_haptics_init(void) {
    if (!device_is_ready(motor)) {
        LOG_ERR("Right haptic device unavailable; keyboard remains usable");
        return 0;
    }
    k_work_queue_start(&motor_queue, motor_stack, K_THREAD_STACK_SIZEOF(motor_stack),
                       K_PRIO_PREEMPT(10), NULL);
    atomic_set(&queue_started, 1);
    k_work_submit_to_queue(&motor_queue, &motor_work);
    return 0;
}
SYS_INIT(ot60_haptics_init, APPLICATION, 90);

static int haptic_event(const zmk_event_t *eh) {
    if (!atomic_get(&queue_started)) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_activity_state_changed *activity = as_zmk_activity_state_changed(eh);
    if (activity) {
        if (activity->state == ZMK_ACTIVITY_SLEEP) {
            struct k_work_sync sync;
            atomic_set(&sleeping, 1);
            atomic_clear(&manual_effect);
            atomic_clear(&manual_busy);
            atomic_clear(&automatic_pending);
            /* Finish I2C cleanup before ZMK suspends the I2C controller. */
            k_work_cancel_sync(&motor_work, &sync);
        } else if (atomic_cas(&sleeping, 1, 0) && atomic_get(&calibrate_pending)) {
            k_work_submit_to_queue(&motor_queue, &motor_work);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_position_state_changed *position = as_zmk_position_state_changed(eh);
    if (position && position->state &&
        position->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL &&
        atomic_get(&ready) && atomic_get(&enabled) && !atomic_get(&sleeping) &&
        !zmk_keymap_layer_active(1) && !atomic_get(&manual_busy)) {
        /* Rapid presses coalesce; no unbounded vibration backlog. */
        atomic_set(&automatic_pending, 1);
        k_work_submit_to_queue(&motor_queue, &motor_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(ot60_haptics, haptic_event);
ZMK_SUBSCRIPTION(ot60_haptics, zmk_position_state_changed);
ZMK_SUBSCRIPTION(ot60_haptics, zmk_activity_state_changed);

static int haptic_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    if (!atomic_get(&queue_started) || atomic_get(&sleeping)) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    switch (binding->param1) {
    case 0:
        atomic_set(&enabled, !atomic_get(&enabled));
        break;
    case 1:
        return request_effect(1) < 0 ? -EAGAIN : ZMK_BEHAVIOR_OPAQUE;
    case 2:
        atomic_clear(&manual_effect);
        atomic_clear(&automatic_pending);
        atomic_set(&calibrate_pending, 1);
        k_work_submit_to_queue(&motor_queue, &motor_work);
        break;
    default:
        return -EINVAL;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int haptic_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata commands[] = {
    {.display_name = "Toggle keypress haptics", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 0},
    {.display_name = "Test click", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 1},
    {.display_name = "Calibrate right LRA", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = 2},
};
static const struct behavior_parameter_metadata_set command_sets[] = {
    {.param1_values = commands, .param1_values_len = ARRAY_SIZE(commands)},
};
static const struct behavior_parameter_metadata metadata = {
    .sets = command_sets, .sets_len = ARRAY_SIZE(command_sets),
};
#endif
static const struct behavior_driver_api haptic_api = {
    .binding_pressed = haptic_pressed,
    .binding_released = haptic_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};
BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &haptic_api);

static int haptic_effect_pressed(struct zmk_behavior_binding *binding,
                                 struct zmk_behavior_binding_event event) {
    int rc = request_effect(binding->param1);
    return rc < 0 ? rc : ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static const struct behavior_parameter_value_metadata effect_values[] = {
    {.display_name = "ROM effect ID (1-123)",
     .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE,
     .range = {.min = HAPTIC_EFFECT_MIN, .max = HAPTIC_EFFECT_MAX}},
};
static const struct behavior_parameter_metadata_set effect_sets[] = {
    {.param1_values = effect_values, .param1_values_len = ARRAY_SIZE(effect_values)},
};
static const struct behavior_parameter_metadata effect_metadata = {
    .sets = effect_sets, .sets_len = ARRAY_SIZE(effect_sets),
};
#endif
static const struct behavior_driver_api haptic_effect_api = {
    .binding_pressed = haptic_effect_pressed,
    .binding_released = haptic_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &effect_metadata,
#endif
};
BEHAVIOR_DT_DEFINE(DT_NODELABEL(haptic_play), NULL, NULL, NULL, NULL, POST_KERNEL,
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &haptic_effect_api);
