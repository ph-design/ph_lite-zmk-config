/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zmk/power.h>
#include <zmk/motion_guard.h>
#include <zmk/usb.h>
#include <zmk/studio/core.h>
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
#include <zmk/rgb_underglow.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
extern int zmk_backlight_power_refresh(void);
#endif

static struct zmk_power_config config = ZMK_POWER_DEFAULTS;
static struct k_spinlock config_lock;
static int64_t last_activity;
static int64_t last_key = -1;
static bool initialized;
static bool held_keys[128];
static unsigned held_count;
static struct power_decision decision;
static void lights_work_cb(struct k_work *work) {
    ARG_UNUSED(work);
#if IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW)
    zmk_rgb_underglow_apply_state();
#endif
#if IS_ENABLED(CONFIG_ZMK_BACKLIGHT)
    zmk_backlight_power_refresh();
#endif
}
static K_WORK_DEFINE(lights_work, lights_work_cb);
static void update_work_cb(struct k_work *work);
static K_WORK_DEFINE(update_work, update_work_cb);

void zmk_power_get_config(struct zmk_power_config *out) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    *out = config;
    k_spin_unlock(&config_lock, key);
}
int zmk_power_set_config(const struct zmk_power_config *requested) {
    if (!power_config_valid(requested)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = *requested;
    k_spin_unlock(&config_lock, key);
    /* Apply on the policy workqueue, never change the activity clock from RPC. */
    k_work_submit(&update_work);
    return 0;
}
struct power_blob { uint32_t version, idle, rgb, backlight, deep, flags; };
int zmk_power_save_config(void) {
    struct zmk_power_config c; zmk_power_get_config(&c);
    struct power_blob blob = {1, c.idle_timeout_ms, c.rgb_off_ms, c.backlight_off_ms,
        c.deep_sleep_ms, (c.on_usb ? 1 : 0) | (c.off_when_locked ? 2 : 0)};
    return settings_save_one("power/v1", &blob, sizeof(blob));
}
int zmk_power_reset_config(void) {
    const struct zmk_power_config defaults = ZMK_POWER_DEFAULTS;
    int rc = settings_delete("power/v1");
    zmk_power_set_config(&defaults);
    return rc == -ENOENT ? 0 : rc;
}
static int power_settings_load(const char *name, size_t len, settings_read_cb read_cb, void *arg) {
    if (strcmp(name, "v1")) return -ENOENT;
    struct power_blob b;
    if (len != sizeof(b) || read_cb(arg, &b, sizeof(b)) != sizeof(b) || b.version != 1 || b.flags > 3) return -EINVAL;
    struct zmk_power_config c = {b.idle, b.rgb, b.backlight, b.deep, (b.flags & 1) != 0, (b.flags & 2) != 0};
    return zmk_power_set_config(&c);
}
SETTINGS_STATIC_HANDLER_DEFINE(zmk_power_settings, "power", NULL, power_settings_load, NULL, NULL);

void zmk_power_update(void) {
    if (!initialized) return;
    struct zmk_power_config c; zmk_power_get_config(&c);
    int64_t now = k_uptime_get();
    if (held_count) last_activity = now;
    bool usb = false;
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    usb = zmk_usb_is_powered();
#endif
    bool studio_unlocked = false;
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
    studio_unlocked = zmk_studio_core_get_lock_state() == ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
#endif
    struct power_decision next = power_decide(&c, now - last_activity, held_count != 0,
        usb, zmk_motion_guard_blocks_output(), studio_unlocked);
    bool rgb_changed = decision.rgb_off != next.rgb_off;
    bool backlight_changed = decision.backlight_off != next.backlight_off;
    decision = next;
    if (rgb_changed || backlight_changed) k_work_submit(&lights_work);
}
void zmk_power_note_activity(void) { last_activity = k_uptime_get(); zmk_power_update(); }
void zmk_power_note_key(uint32_t position, bool down) {
    last_key = k_uptime_get();
    if (position < ARRAY_SIZE(held_keys) && held_keys[position] != down) {
        held_keys[position] = down;
        if (down) held_count++; else if (held_count) held_count--;
    }
    zmk_power_note_activity();
}
static void update_work_cb(struct k_work *work) { ARG_UNUSED(work); zmk_power_note_activity(); }
bool zmk_power_idle(void) { return decision.idle; }
bool zmk_power_deep_sleep(void) { return decision.deep_sleep; }
bool zmk_power_rgb_off(void) { return decision.rgb_off; }
bool zmk_power_backlight_off(void) { return decision.backlight_off; }
static int power_init(void) { last_activity = k_uptime_get(); initialized = true; return 0; }
/* RGB and backlight initialize at CONFIG_APPLICATION_INIT_PRIORITY first. */
BUILD_ASSERT(CONFIG_APPLICATION_INIT_PRIORITY < 99);
SYS_INIT(power_init, APPLICATION, 99);

bool zmk_power_input_recent(uint32_t quiet_ms) { return held_count || (last_key >= 0 && k_uptime_get() - last_key < quiet_ms); }
