/* SPDX-License-Identifier: MIT
 * I2C only in sampler thread. Policy/replay/kscan on system workqueue.
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zmk/endpoints.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/motion_guard.h>
#include <zmk/power.h>
#include <zmk/tap.h>
#if IS_ENABLED(CONFIG_ZMK_2G4)
#include <zmk/2g4.h>
#endif
#include "guard.h"

LOG_MODULE_REGISTER(motion_guard, CONFIG_ZMK_LOG_LEVEL);
static const struct device *imu = DEVICE_DT_GET(DT_CHOSEN(zmk_imu));
static struct mg_guard guard;
static struct mg_config requested_config = MG_CONFIG_DEFAULTS;
static struct k_spinlock config_lock;
static uint32_t config_revision, applied_revision;
void zmk_motion_guard_get_config(struct mg_config *config) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    *config = requested_config;
    k_spin_unlock(&config_lock, key);
}
int zmk_motion_guard_set_config(const struct mg_config *config) {
    if (!mg_config_valid(config)) { return -EINVAL; }
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    requested_config = *config;
    config_revision++;
    k_spin_unlock(&config_lock, key);
    return 0;
}
struct guard_blob { uint32_t version; struct mg_config config; };
int zmk_motion_guard_save_config(void) {
    struct guard_blob blob = {.version = 1};
    zmk_motion_guard_get_config(&blob.config);
    return settings_save_one("motion_guard/v1", &blob, sizeof(blob));
}
int zmk_motion_guard_reset_config(void) {
    const struct mg_config defaults = MG_CONFIG_DEFAULTS;
    int rc = settings_delete("motion_guard/v1");
    zmk_motion_guard_set_config(&defaults);
    return rc == -ENOENT ? 0 : rc;
}
static int guard_settings_load(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (strcmp(name, "v1")) { return -ENOENT; }
    struct guard_blob blob;
    if (len != sizeof(blob) || read_cb(arg, &blob, sizeof(blob)) != sizeof(blob) ||
        blob.version != 1 || !mg_config_valid(&blob.config)) { return -EINVAL; }
    return zmk_motion_guard_set_config(&blob.config);
}
SETTINGS_STATIC_HANDLER_DEFINE(motion_guard_settings, "motion_guard", NULL, guard_settings_load, NULL, NULL);

static bool initialized, clearing;
static atomic_t blocked = 1, requested_enabled = 1, healthy, generation, started;
extern void zmk_activity_motion_active(void);
struct sample_frame {
    struct sensor_value xyz[3];
    int64_t time;
    uint32_t sequence;
    bool ok;
};
static K_MUTEX_DEFINE(sampler_mutex);
static struct sample_frame latest;
static struct k_spinlock frame_lock;
static uint32_t processed_sequence;
static atomic_t tap_odr;
static void policy_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(policy_work, policy_work_cb);
static void drain_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(drain_work, drain_work_cb);

static void emit_key(struct mg_key key, void *context) {
    ARG_UNUSED(context);
    atomic_set(&blocked, mg_blocks(&guard));
    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .position = key.position, .state = key.down, .timestamp = key.time});
}
static void ensure_initialized(void) {
    if (!initialized) { mg_init(&guard, emit_key, NULL); initialized = true; }
}
static struct sample_frame read_frame(void) {
    k_spinlock_key_t key = k_spin_lock(&frame_lock);
    struct sample_frame frame = latest;
    k_spin_unlock(&frame_lock, key);
    return frame;
}
int zmk_motion_guard_read_accel(struct sensor_value *xyz) {
    struct sample_frame frame = read_frame();
    if (!frame.ok || k_uptime_get() - frame.time > MG_STALE_MS) { return -EIO; }
    memcpy(xyz, frame.xyz, sizeof(frame.xyz));
    return 0;
}
static void synchronize(enum mg_state previous) {
    bool was_blocked = guard.enabled && (previous == MG_CARRY || previous == MG_SETTLING);
    bool is_blocked = mg_blocks(&guard);
    if (!was_blocked && is_blocked) { atomic_inc(&generation); }
    atomic_set(&blocked, is_blocked);
    if (guard.state == MG_CARRY && previous != MG_CARRY) {
        clearing = true;
        zmk_endpoints_clear_current();
#if IS_ENABLED(CONFIG_ZMK_POINTING)
        zmk_endpoints_send_mouse_report();
#endif
        clearing = false;
    }
    if (was_blocked != is_blocked) {
#if IS_ENABLED(CONFIG_ZMK_2G4)
        zmk_2g4_motion_state_changed();
#endif
        zmk_power_update();
        LOG_INF("guard %s", is_blocked ? "LOCK" : "READY");
        if (!is_blocked) { zmk_activity_motion_active(); }
    }
    if (guard.queued && !is_blocked) { k_work_schedule(&drain_work, K_NO_WAIT); }
}
static void update_policy(void) {
    ensure_initialized();
    enum mg_state previous = guard.state;
    k_spinlock_key_t config_key = k_spin_lock(&config_lock);
    bool changed = applied_revision != config_revision;
    guard.config = requested_config;
    applied_revision = config_revision;
    k_spin_unlock(&config_lock, config_key);
    if (changed && mg_blocks(&guard)) { mg_lock(&guard, k_uptime_get()); }
    bool enabled = atomic_get(&requested_enabled);
    mg_enable(&guard, enabled);
    struct sample_frame frame = read_frame();
    int64_t now = k_uptime_get();
    if (frame.ok && now - frame.time <= MG_STALE_MS) {
        if (frame.sequence != processed_sequence) {
            processed_sequence = frame.sequence;
            int32_t mg[3];
            for (unsigned i = 0; i < 3; i++) {
                int64_t micro = (int64_t)frame.xyz[i].val1 * 1000000 + frame.xyz[i].val2;
                mg[i] = (int32_t)(micro * 1000 / 9806650);
            }
#if IS_ENABLED(CONFIG_ZMK_MOTION_IMU_FLIP_Z)
            mg[2] = -mg[2];
#endif
            mg_sample(&guard, frame.time, mg[0], mg[1], mg[2]);
        }
        atomic_set(&healthy, 1);
    } else if (!guard.valid || now - guard.last_sample > MG_STALE_MS) {
        atomic_set(&healthy, 0);
        if (enabled) { mg_lock(&guard, now); }
    }
    synchronize(previous);
}
void zmk_motion_guard_key(uint32_t position, bool down, int64_t time) {
    update_policy(); /* Cached RAM only. No I2C. */
    enum mg_state previous = guard.state;
    mg_key(&guard, (struct mg_key){position, down, time});
    synchronize(previous);
}
static void drain_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    update_policy();
    mg_drain(&guard, k_uptime_get());
    if (guard.queued && !mg_blocks(&guard)) {
        k_work_reschedule(&drain_work, K_MSEC(4));
    }
}
static void policy_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    update_policy();
    k_work_schedule(&policy_work, K_MSEC(10));
}
static void sampler_thread(void *a, void *b, void *c) {
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
    while (true) {
        if (atomic_get(&started)) {
            k_mutex_lock(&sampler_mutex, K_FOREVER);
            if (!atomic_get(&started)) { k_mutex_unlock(&sampler_mutex); continue; }
            struct sample_frame frame = {0};
            if (device_is_ready(imu)) {
                frame.ok = sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ) == 0 &&
                           sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, frame.xyz) == 0;
            }
            frame.time = k_uptime_get();
            if (frame.ok) zmk_tap_sample(frame.xyz);
            static int64_t last_guard_frame;
            if (frame.time - last_guard_frame >= 10) {
            last_guard_frame = frame.time;
            k_spinlock_key_t key = k_spin_lock(&frame_lock);
            frame.sequence = latest.sequence + 1;
            latest = frame;
            k_spin_unlock(&frame_lock, key);
            k_work_schedule(&policy_work, K_NO_WAIT);
            }
            k_mutex_unlock(&sampler_mutex);
        }
        if (zmk_tap_test_active()) k_usleep(2500); else k_sleep(K_MSEC(10));
    }
}
int zmk_motion_guard_set_tap_odr(bool enabled) {
    k_mutex_lock(&sampler_mutex, K_FOREVER);
    const struct sensor_value odr = {.val1 = enabled ? 400 : 100};
    int rc = device_is_ready(imu) ? sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr) : -ENODEV;
    if (!rc) atomic_set(&tap_odr, enabled);
    k_mutex_unlock(&sampler_mutex);
    return rc;
}
void zmk_motion_guard_prepare_sleep(void) {
    atomic_set(&started, 0);
    /* Wait for the final in-flight bus read before device suspension. */
    k_mutex_lock(&sampler_mutex, K_FOREVER);
    k_mutex_unlock(&sampler_mutex);
    k_work_cancel_delayable(&policy_work);
    k_work_cancel_delayable(&drain_work);
}
void zmk_motion_guard_resume_sampling(void) {
    atomic_set(&started, 1);
    k_work_schedule(&policy_work, K_MSEC(10));
}
K_THREAD_DEFINE(motion_guard_sampler, 1536, sampler_thread, NULL, NULL, NULL, 8, 0, 0);
bool zmk_motion_guard_blocks_output(void) { return atomic_get(&blocked); }
bool zmk_motion_guard_clearing(void) { return clearing; }
uint32_t zmk_motion_guard_generation(void) { return (uint32_t)atomic_get(&generation); }
bool zmk_motion_guard_keeps_awake(void) {
    return atomic_get(&requested_enabled) && atomic_get(&healthy);
}
void zmk_motion_guard_set_enabled(bool enabled) { atomic_set(&requested_enabled, enabled); }
static int guard_init(void) {
    ensure_initialized();
    if (device_is_ready(imu)) {
        const struct sensor_value odr = {.val1 = 100, .val2 = 0};
        int rc = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
                                 SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
        if (rc) { LOG_WRN("100Hz ODR request failed: %d", rc); }
    } else { LOG_ERR("IMU unavailable; disable carry in Studio to recover input"); }
    atomic_set(&started, 1);
    LOG_INF("EVO Motion Guard preview.7: hardware tap modes and diagnostics");
    k_work_schedule(&policy_work, K_NO_WAIT);
    return 0;
}
SYS_INIT(guard_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
