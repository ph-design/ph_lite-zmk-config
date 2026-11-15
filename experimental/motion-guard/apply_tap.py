"""Run only after integration.patch on the pinned isolated core checkout."""
from pathlib import Path
import sys
root = Path(sys.argv[1])
p = root / "app/src/motion.c"
s = p.read_text()
s = s.replace('#include <zmk/motion_guard.h>', '#include <zmk/motion_guard.h>\n#include <zmk/tap.h>')
s = s.replace('enum tap_side { TAP_LEFT, TAP_RIGHT, TAP_SIDE_COUNT };', 'static K_MUTEX_DEFINE(tap_config_mutex);')
s = s.replace('.time_limit_ms = 120,', '.time_limit_ms = 30,')
start = s.index('// per-side single-tap state')
end = s.index('static void carry_check_work_cb(struct k_work *work) {', start)
s = s[:start] + '''static bool binding_is_set(const struct zmk_behavior_binding *b) { return b && b->behavior_dev != NULL; }
static void motion_live_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(live_work, motion_live_work_cb);
static void carry_check_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(carry_work, carry_check_work_cb);
''' + s[end:]
s = s.replace('''    int rc;

    uint32_t ths_reg''', '''    int rc = zmk_motion_guard_set_tap_odr(tap_cfg.enabled);
    if (rc) return rc;

    uint32_t ths_reg''', 1)
start = s.index('static void apply_tap_triggers(void) {')
end = s.index('static int apply_sleep_wake_hw(void)', start)
s = s[:start] + '''static int apply_tap_triggers(void) { return zmk_tap_rearm(imu, &tap_cfg); }

''' + s[end:]
start = s.index('int zmk_motion_get_tap_config(')
end = s.index('int zmk_motion_get_carry_config(', start)
s = s[:start] + '''int zmk_motion_get_tap_config(struct zmk_motion_tap_config *out) {
    k_mutex_lock(&tap_config_mutex, K_FOREVER); *out = tap_cfg; k_mutex_unlock(&tap_config_mutex);
    return 0;
}
int zmk_motion_set_tap_config(const struct zmk_motion_tap_config *cfg) {
    if (cfg->threshold > 127 || cfg->time_limit_ms < 1 || cfg->time_limit_ms > 317 ||
        cfg->latency_ms < 1 || cfg->latency_ms > 637 || cfg->window_ms < 1 || cfg->window_ms > 637) return -EINVAL;
    k_mutex_lock(&tap_config_mutex, K_FOREVER);
    struct zmk_motion_tap_config previous = tap_cfg; tap_cfg = *cfg;
    int rc = apply_tap_hw();
    if (!rc) rc = apply_tap_triggers();
    if (rc) { tap_cfg = previous; apply_tap_hw(); apply_tap_triggers(); }
    k_mutex_unlock(&tap_config_mutex);
    return rc;
}

''' + s[end:]
s = s.replace('static int motion_settings_commit(void) {', '''static int motion_settings_commit(void) {
    /* Migrate old timing values into the 400 Hz hardware register range in RAM. */
    tap_cfg.time_limit_ms = CLAMP(tap_cfg.time_limit_ms, 1, 317);
    tap_cfg.latency_ms = CLAMP(tap_cfg.latency_ms, 1, 637);
    tap_cfg.window_ms = CLAMP(tap_cfg.window_ms, 1, 637);''')
assert 'pending_singles' not in s and 'TAP_SIDE_COUNT' not in s
p.write_bytes(s.encode())
