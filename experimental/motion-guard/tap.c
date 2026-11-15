/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/sensor/lis2dh.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/tap.h>
#include <zmk/motion_guard.h>
#include <zmk/power.h>
#include <zmk/imu_game.h>
#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/virtual_key_position.h>
#include <zmk/events/position_state_changed.h>
enum tap_gesture { TAP_NONE, TAP_SINGLE, TAP_DOUBLE };

static struct zmk_tap_options options = {7, 120, false};
static struct k_spinlock lock;
static struct zmk_tap_diagnostics diagnostics;
static struct zmk_tap_capture captured;
static uint8_t ring[32][8];
static unsigned ring_next, ring_count;
static atomic_t config_generation, test_until;
static uint32_t last_generation, last_guard;
static int64_t last_double;
static bool have_double;
static const struct device *sensor;
struct tap_event { uint32_t config, guard; int64_t time; uint8_t src; bool dry; };
K_MSGQ_DEFINE(tap_events, sizeof(struct tap_event), 8, 4);
static void raw_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(raw_work, raw_work_cb);
static void test_expiry_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(test_expiry, test_expiry_cb);

static void suppressed(void) { k_spinlock_key_t k = k_spin_lock(&lock); diagnostics.suppressed++; k_spin_unlock(&lock, k); }
void zmk_tap_get_options(struct zmk_tap_options *out) { k_spinlock_key_t k = k_spin_lock(&lock); *out = options; k_spin_unlock(&lock, k); }
bool zmk_tap_options_valid(const struct zmk_tap_options *o) { return o->axes >= 1 && o->axes <= 7 && o->quiet_ms <= 1000; }
bool zmk_tap_test_active(void) { uint32_t until = atomic_get(&test_until); return until && (int32_t)(until - k_uptime_get_32()) > 0; }
static bool bound(const struct zmk_behavior_binding *b) { return b->behavior_dev != NULL; }
static const struct zmk_behavior_binding *binding(const struct zmk_motion_tap_config *c, unsigned side, bool dbl) {
    return side ? (dbl ? &c->right_double : &c->right_single) : (dbl ? &c->left_double : &c->left_single);
}
static bool eligible(const struct zmk_motion_tap_config *c, const struct zmk_tap_options *o) {
    return c->enabled && !zmk_imu_game_active() && !zmk_motion_guard_blocks_output() && !zmk_power_input_recent(o->quiet_ms) &&
        (!c->layer_mask || (zmk_keymap_layer_state() & c->layer_mask));
}
static void emit(enum tap_gesture gesture, unsigned side, const struct zmk_motion_tap_config *c,
                 const struct zmk_tap_options *o, uint32_t cg, uint32_t gg, bool dry) {
    if (gesture != TAP_DOUBLE) return; /* Stored legacy single bindings never execute. */
    if (cg != (uint32_t)atomic_get(&config_generation) || gg != zmk_motion_guard_generation() || !eligible(c, o)) { suppressed(); return; }
    k_spinlock_key_t key = k_spin_lock(&lock);
    diagnostics.gestures++; diagnostics.gesture = gesture;
    k_spin_unlock(&lock, key);
    if (dry || zmk_tap_test_active()) return;
    const struct zmk_behavior_binding *b = binding(c, side, gesture == TAP_DOUBLE);
    if (!bound(b)) return;
    struct zmk_behavior_binding_event event = {.position = ZMK_VIRTUAL_KEY_POSITION_MOTION(side), .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    zmk_power_note_activity();
    zmk_behavior_invoke_binding(b, event, true);
    zmk_behavior_invoke_binding(b, event, false);
    key = k_spin_lock(&lock); diagnostics.actions++; k_spin_unlock(&lock, key);
}
static void raw_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
    struct tap_event e;
    while (!k_msgq_get(&tap_events, &e, K_NO_WAIT)) {
        uint32_t generation = atomic_get(&config_generation);
        struct zmk_motion_tap_config c; zmk_motion_get_tap_config(&c);
        struct zmk_tap_options o; zmk_tap_get_options(&o);
        if (!(e.src & 0x20) || e.config != generation || e.guard != zmk_motion_guard_generation() || !eligible(&c, &o)) { suppressed(); continue; }
        if (have_double && last_generation == generation && last_guard == e.guard && e.time - last_double < c.latency_ms) { suppressed(); continue; }
        last_double = e.time; last_generation = generation; last_guard = e.guard; have_double = true;
        bool negative = (e.src & 8) != 0;
#if IS_ENABLED(CONFIG_ZMK_MOTION_TAP_SIDE_INVERT)
        negative = !negative;
#endif
        unsigned side = o.split && !negative ? 1 : 0;
        emit(TAP_DOUBLE, side, &c, &o, generation, e.guard, e.dry);
    }
}
static void irq_handler(const struct device *dev, const struct sensor_trigger *trigger) {
    ARG_UNUSED(trigger);
    struct sensor_value src;
    if (sensor_channel_get(dev, SENSOR_CHAN_LIS2DH_CLICK_SRC, &src)) return;
    struct tap_event e = {.src = src.val1, .time = k_uptime_get(), .guard = zmk_motion_guard_generation(), .config = atomic_get(&config_generation), .dry = zmk_tap_test_active()};
    if (k_msgq_put(&tap_events, &e, K_NO_WAIT)) {
        k_spinlock_key_t k = k_spin_lock(&lock); diagnostics.drops++; k_spin_unlock(&lock, k);
        atomic_inc(&config_generation); /* Never replay a partial gesture after overflow. */
    } else k_work_schedule(&raw_work, K_MSEC(12));
}
int zmk_tap_rearm(const struct device *imu, const struct zmk_motion_tap_config *c) {
    sensor = imu;
    atomic_inc(&config_generation);
    k_msgq_purge(&tap_events);
    if (!c->enabled) { atomic_set(&test_until, 0); k_work_cancel_delayable(&test_expiry); }
    struct zmk_tap_options o; zmk_tap_get_options(&o);
    /* Always use the chip double-click engine, including no-action tests. */
    bool hardware_double = true;
    static const struct sensor_trigger single = {.type = SENSOR_TRIG_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ};
    static const struct sensor_trigger dbl = {.type = SENSOR_TRIG_DOUBLE_TAP, .chan = SENSOR_CHAN_ACCEL_XYZ};
    int rc = sensor_trigger_set(imu, &single, c->enabled && !hardware_double ? irq_handler : NULL);
    if (!rc) rc = sensor_trigger_set(imu, &dbl, c->enabled && hardware_double ? irq_handler : NULL);
    if (!rc) rc = zmk_tap_sensor_configure(imu, c->enabled ? o.axes : 0, hardware_double);
    k_spinlock_key_t k = k_spin_lock(&lock); diagnostics.error = rc; diagnostics.engine = c->enabled ? (hardware_double ? 2 : 1) : 0; k_spin_unlock(&lock, k);
    return rc;
}
int zmk_tap_set_options(const struct zmk_tap_options *o) {
    if (!zmk_tap_options_valid(o)) return -EINVAL;
    struct zmk_tap_options previous; zmk_tap_get_options(&previous);
    k_spinlock_key_t k = k_spin_lock(&lock); options = *o; k_spin_unlock(&lock, k);
    if (!sensor) return 0;
    struct zmk_motion_tap_config c; zmk_motion_get_tap_config(&c);
    int rc = zmk_tap_rearm(sensor, &c);
    if (rc) { k = k_spin_lock(&lock); options = previous; k_spin_unlock(&lock, k); zmk_tap_rearm(sensor, &c); }
    return rc;
}
int zmk_tap_set_test(bool enabled) {
    struct zmk_motion_tap_config c; zmk_motion_get_tap_config(&c);
    if (!sensor || (enabled && !c.enabled)) return -EACCES;
    if (enabled) {
        k_spinlock_key_t k = k_spin_lock(&lock);
        uint8_t regs[11]; memcpy(regs, diagnostics.registers, sizeof(regs));
        uint32_t odr = diagnostics.odr, fs = diagnostics.full_scale_mg;
        diagnostics = (struct zmk_tap_diagnostics){.odr = odr, .full_scale_mg = fs};
        memcpy(diagnostics.registers, regs, sizeof(regs)); captured.count = 0;
        k_spin_unlock(&lock, k);
        atomic_set(&test_until, (atomic_val_t)(k_uptime_get_32() + 60000));
        k_work_reschedule(&test_expiry, K_SECONDS(60));
    } else { atomic_set(&test_until, 0); k_work_cancel_delayable(&test_expiry); }
    return zmk_tap_rearm(sensor, &c);
}
static void test_expiry_cb(struct k_work *work) { ARG_UNUSED(work); zmk_tap_set_test(false); }
void zmk_tap_get_diagnostics(struct zmk_tap_diagnostics *out) {
    k_spinlock_key_t k = k_spin_lock(&lock); *out = diagnostics; k_spin_unlock(&lock, k);
    int32_t remaining = (int32_t)((uint32_t)atomic_get(&test_until) - k_uptime_get_32());
    out->test_ms = atomic_get(&test_until) && remaining > 0 ? remaining : 0;
}
void zmk_tap_driver_irq(uint8_t src) {
    k_spinlock_key_t k = k_spin_lock(&lock);
    diagnostics.src = src;
    if (src & 0x10) diagnostics.single++;
    if (src & 0x20) diagnostics.dbl++;
    for (unsigned i = 0; i < 3; i++) if ((src & 0x30) && (src & BIT(i))) diagnostics.axes[i]++;
    if (src & 0x30) {
        captured.sequence++; captured.count = ring_count;
        for (unsigned i = 0; i < ring_count; i++) memcpy(captured.frames + i * 8, ring[(ring_next + 32 - ring_count + i) % 32], 8);
    }
    k_spin_unlock(&lock, k);
}
void zmk_tap_driver_registers(const uint8_t *registers, uint32_t odr, uint32_t fs_mg) {
    k_spinlock_key_t k = k_spin_lock(&lock);
    memcpy(diagnostics.registers, registers, 11); diagnostics.odr = odr; diagnostics.full_scale_mg = fs_mg;
    k_spin_unlock(&lock, k);
}
void zmk_tap_sample(const struct sensor_value *xyz) {
    int32_t values[3];
    uint8_t frame[8]; sys_put_le16((uint16_t)k_uptime_get_32(), frame);
    for (unsigned i = 0; i < 3; i++) {
        int64_t micro = (int64_t)xyz[i].val1 * 1000000 + xyz[i].val2;
        int32_t mg = micro * 1000 / 9806650;
        values[i] = mg;
        sys_put_le16((uint16_t)(int16_t)CLAMP(mg, -32768, 32767), frame + 2 + i * 2);
    }
    k_spinlock_key_t k = k_spin_lock(&lock); memcpy(diagnostics.current, values, sizeof(values)); memcpy(ring[ring_next], frame, 8); ring_next = (ring_next + 1) % 32; if (ring_count < 32) ring_count++; k_spin_unlock(&lock, k);
}
void zmk_tap_get_capture(struct zmk_tap_capture *out) { k_spinlock_key_t k = k_spin_lock(&lock); *out = captured; k_spin_unlock(&lock, k); }
struct tap_blob { uint32_t version, axes, quiet, split; };
int zmk_tap_save_options(void) { struct zmk_tap_options o; zmk_tap_get_options(&o); struct tap_blob b = {1, o.axes, o.quiet_ms, o.split}; return settings_save_one("tap/v1", &b, sizeof(b)); }
int zmk_tap_reset(void) { const struct zmk_tap_options defaults = {7, 120, false}; int rc = settings_delete("tap/v1"); zmk_tap_set_options(&defaults); zmk_tap_set_test(false); return rc == -ENOENT ? 0 : rc; }
static int tap_load(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (strcmp(name, "v1")) return -ENOENT;
    struct tap_blob b; if (len != sizeof(b) || read_cb(arg, &b, sizeof(b)) != sizeof(b) || b.version != 1 || b.split > 1) return -EINVAL;
    struct zmk_tap_options o = {b.axes, b.quiet, b.split != 0}; return zmk_tap_set_options(&o);
}
SETTINGS_STATIC_HANDLER_DEFINE(zmk_tap_settings, "tap", NULL, tap_load, NULL, NULL);

void zmk_tap_driver_gpio(void) { k_spinlock_key_t k = k_spin_lock(&lock); diagnostics.irq++; k_spin_unlock(&lock, k); }
