/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>
#include "hub.h"

enum led_mode { LED_AUTO, LED_ON, LED_OFF };

struct led_config {
    uint8_t mode;
    uint8_t brightness;
    uint8_t threshold;
    uint16_t period;
};

#define LED_DEFAULTS {LED_AUTO, 100, 20, 2000}

static const char *const modes[] = {"AUTO", "ON", "OFF"};
static const struct pwm_dt_spec led = PWM_DT_SPEC_GET(DT_NODELABEL(status_led));
static struct led_config config = LED_DEFAULTS;
static struct led_config saved = LED_DEFAULTS;
static struct k_spinlock config_lock;
static int64_t identify_until;

static bool config_valid(const struct led_config *cfg) {
    return cfg->mode < ARRAY_SIZE(modes) && cfg->brightness <= 100 && cfg->threshold >= 5 &&
           cfg->threshold <= 50 && cfg->period >= 500 && cfg->period <= 5000;
}

static int led_load(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name, "config")) {
        return -ENOENT;
    }
    uint8_t data[6];
    if (len != sizeof(data) || read(arg, data, len) != len || data[0] != 1) {
        return -EINVAL;
    }
    struct led_config loaded = {data[1], data[2], data[3], sys_get_le16(&data[4])};
    if (!config_valid(&loaded)) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = saved = loaded;
    k_spin_unlock(&config_lock, key);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(dongle_led, "dongle_led", NULL, led_load, NULL, NULL);

int hub_led_configure(const char *args) {
    const char *separator = strchr(args, ',');
    if (!separator) {
        return -EINVAL;
    }
    size_t mode_len = separator - args;
    unsigned int mode;
    for (mode = 0; mode < ARRAY_SIZE(modes); mode++) {
        if (strlen(modes[mode]) == mode_len && !strncasecmp(modes[mode], args, mode_len)) {
            break;
        }
    }

    unsigned long values[3];
    for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
        const char *start = separator + 1;
        char *end;
        if (*start < '0' || *start > '9') {
            return -EINVAL;
        }
        values[i] = strtoul(start, &end, 10);
        if (*end != (i + 1 == ARRAY_SIZE(values) ? '\0' : ',') || values[i] > 5000) {
            return -EINVAL;
        }
        separator = end;
    }
    if (values[0] > 100 || values[1] > 50) {
        return -EINVAL;
    }
    struct led_config requested = {mode, values[0], values[1], values[2]};
    if (!config_valid(&requested)) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    config = requested;
    k_spin_unlock(&config_lock, key);
    return 0;
}

int hub_led_save(void) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    struct led_config snapshot = config;
    k_spin_unlock(&config_lock, key);

    uint8_t data[6] = {1, snapshot.mode, snapshot.brightness, snapshot.threshold};
    sys_put_le16(snapshot.period, &data[4]);
    int err = settings_save_one("dongle_led/config", data, sizeof(data));
    if (!err) {
        key = k_spin_lock(&config_lock);
        saved = snapshot;
        k_spin_unlock(&config_lock, key);
    }
    return err;
}

void hub_led_identify(void) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    identify_until = k_uptime_get() + 5000;
    k_spin_unlock(&config_lock, key);
}

void hub_led_status(char *out, size_t size) {
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    struct led_config cfg = config;
    bool dirty = cfg.mode != saved.mode || cfg.brightness != saved.brightness ||
                 cfg.threshold != saved.threshold || cfg.period != saved.period;
    k_spin_unlock(&config_lock, key);

    snprintk(out, size,
             "LED_CAPS=1\r\nLED_MODE=%s\r\nLED_BRIGHTNESS=%u\r\nLED_LOW_PCT=%u\r\n"
             "LED_PERIOD_MS=%u\r\nLED_DIRTY=%d\r\n",
             modes[cfg.mode], cfg.brightness, cfg.threshold, cfg.period, dirty);
}

static uint8_t automatic_brightness(const struct led_config *cfg, int64_t now) {
    static bool low, was_connected;
    static uint8_t last_threshold;
    static int64_t connected_at;
    bool connected = hub_keyboard_connected();
    int battery = hub_keyboard_battery();
    uint32_t phase = (now % cfg->period) * 2000 / cfg->period;

    if (connected && !was_connected) {
        connected_at = now;
    }
    was_connected = connected;
    if (last_threshold != cfg->threshold) {
        last_threshold = cfg->threshold;
        low = false;
    }
    low = connected && battery >= 0 &&
          (low ? battery < cfg->threshold + 5 : battery <= cfg->threshold);

    bool on;
    if (!connected) {
        on = phase < 1000;
    } else if (now - connected_at < 500) {
        on = (now - connected_at) % 250 < 125;
    } else if (low) {
        on = phase < 100 || (phase >= 250 && phase < 350);
    } else if (hub_keyboard_locked()) {
        uint32_t ramp = phase < 1000 ? phase : 2000 - phase;
        return cfg->brightness * (5 + 95 * ramp * ramp / 1000000) / 100;
    } else {
        on = battery >= 0 || phase < 100;
    }
    return on ? cfg->brightness : 0;
}

static void led_update(struct k_work *work) {
    int64_t now = k_uptime_get();
    k_spinlock_key_t key = k_spin_lock(&config_lock);
    struct led_config cfg = config;
    bool identify = now < identify_until;
    k_spin_unlock(&config_lock, key);

    uint8_t auto_level = automatic_brightness(&cfg, now);
    uint8_t level = cfg.mode == LED_AUTO ? auto_level : cfg.mode == LED_ON ? cfg.brightness : 0;
    if (identify) {
        level = now % 250 < 125 ? 100 : 0;
    }
    pwm_set_pulse_dt(&led, led.period * level / 100);
    k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(25));
}

static K_WORK_DELAYABLE_DEFINE(led_work, led_update);

static int led_init(void) {
    if (!pwm_is_ready_dt(&led)) {
        return -ENODEV;
    }
    int err = pwm_set_pulse_dt(&led, 0);
    if (err) {
        return err;
    }
    k_work_schedule(&led_work, K_NO_WAIT);
    return 0;
}

SYS_INIT(led_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
