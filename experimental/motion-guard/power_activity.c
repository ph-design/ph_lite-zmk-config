/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/motion_guard.h>
#include <zmk/power.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/split_peripheral_layer_changed.h>
#include <zmk/events/sensor_event.h>

#include <zmk/pm.h>

#include <zmk/activity.h>
#include <zmk/watchdog.h>

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/usb.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)
#include <zephyr/input/input.h>
#endif

bool is_usb_power_present(void) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    return zmk_usb_is_powered();
#else
    return false;
#endif /* IS_ENABLED(CONFIG_USB_DEVICE_STACK) */
}

static enum zmk_activity_state activity_state;

static atomic_t force_sleep;

int raise_event(void) {
    return raise_zmk_activity_state_changed(
        (struct zmk_activity_state_changed){.state = activity_state});
}

int set_state(enum zmk_activity_state state) {
    if (activity_state == state)
        return 0;

    activity_state = state;
    return raise_event();
}

enum zmk_activity_state zmk_activity_get_state(void) { return activity_state; }

static int note_activity(void) {
    zmk_power_note_activity();

    return set_state(ZMK_ACTIVITY_ACTIVE);
}

void zmk_activity_motion_active(void) { note_activity(); }

static int activity_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev) { zmk_power_note_key(ev->position, ev->state); return set_state(ZMK_ACTIVITY_ACTIVE); }
    return note_activity();
}

void activity_work_handler(struct k_work *work) {
    zmk_power_update();
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    bool forced = atomic_cas(&force_sleep, 1, 0);
    if ((forced || zmk_power_deep_sleep()) && !is_usb_power_present()) {
        zmk_motion_guard_prepare_sleep();
        set_state(ZMK_ACTIVITY_SLEEP);
        if (zmk_pm_suspend_devices() < 0) {
            LOG_ERR("Failed to suspend all the devices");
            zmk_pm_resume_devices();
            zmk_motion_guard_resume_sampling();
            return;
        }
        zmk_wdt_feed_now();
        sys_poweroff();
        return;
    }
#endif
    set_state(zmk_power_idle() ? ZMK_ACTIVITY_IDLE : ZMK_ACTIVITY_ACTIVE);
}

K_WORK_DEFINE(activity_work, activity_work_handler);

void zmk_activity_force_sleep(void) {
#if IS_ENABLED(CONFIG_ZMK_SLEEP)
    atomic_set(&force_sleep, 1);
    k_work_submit(&activity_work);
#endif
}

void activity_expiry_function(struct k_timer *_timer) { k_work_submit(&activity_work); }

K_TIMER_DEFINE(activity_timer, activity_expiry_function, NULL);

static int activity_init(void) {

    k_timer_start(&activity_timer, K_SECONDS(1), K_SECONDS(1));
    return 0;
}

ZMK_LISTENER(activity, activity_event_listener);
ZMK_SUBSCRIPTION(activity, zmk_position_state_changed);
ZMK_SUBSCRIPTION(activity, zmk_sensor_event);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_SUBSCRIPTION(activity, zmk_split_peripheral_layer_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_POINTING)

static void note_activity_work_cb(struct k_work *_work) { note_activity(); }

K_WORK_DEFINE(note_activity_work, note_activity_work_cb);

static void activity_input_listener(struct input_event *ev) { k_work_submit(&note_activity_work); }

INPUT_CALLBACK_DEFINE(NULL, activity_input_listener);

#endif

SYS_INIT(activity_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
