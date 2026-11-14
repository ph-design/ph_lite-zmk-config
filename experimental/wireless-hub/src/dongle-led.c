#include <zephyr/device.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zmk/2g4.h>
#include "led-control.h"
#include "led-config.h"
#include "led-pattern.h"
#include "led-link.h"
#include "led-motion.h"
#include "hub-peer.h"

static const struct pwm_dt_spec led = PWM_DT_SPEC_GET(DT_NODELABEL(dongle_status_led));
static struct led_config config = {0,100,20,2000}, saved = {0,100,20,2000};
static struct k_spinlock lock;
static struct k_work_delayable led_work;
static int64_t identify_until;
static bool low;
static int previous_threshold = 20;
static struct led_link_state link_state;
static int settings_set(const char *name, size_t len, settings_read_cb read, void *arg) {
    if (strcmp(name,"config")) return -ENOENT;
    uint8_t data[6];
    if (len != sizeof(data)) return -EINVAL;
    int rc = read(arg,data,sizeof(data));
    if (rc < 0) return rc;
    if (rc != sizeof(data) || data[0] != 1) return -EINVAL;
    struct led_config c = {data[1],data[2],data[3],sys_get_le16(&data[4])};
    if (!led_config_valid(&c)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&lock); config = saved = c; k_spin_unlock(&lock,key);
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(dongle_led,"dongle_led",NULL,settings_set,NULL,NULL);
int dongle_led_configure(const char *args) {
    struct led_config c;
    if (led_config_parse(args,&c)) return -EINVAL;
    k_spinlock_key_t key = k_spin_lock(&lock); config = c; k_spin_unlock(&lock,key);
    return 0;
}
int dongle_led_save(void) {
    k_spinlock_key_t key = k_spin_lock(&lock); struct led_config c = config; k_spin_unlock(&lock,key);
    uint8_t data[6] = {1,c.mode,c.brightness,c.threshold,0,0}; sys_put_le16(c.period,&data[4]);
    int rc = settings_save_one("dongle_led/config",data,sizeof(data));
    if (!rc) { key = k_spin_lock(&lock); saved = c; k_spin_unlock(&lock,key); }
    return rc;
}
void dongle_led_identify(void) {
    k_spinlock_key_t key = k_spin_lock(&lock); identify_until = k_uptime_get()+5000; k_spin_unlock(&lock,key);
}
void dongle_led_status(char *out,size_t size) {
    k_spinlock_key_t key = k_spin_lock(&lock); struct led_config c = config, s = saved; k_spin_unlock(&lock,key);
    bool dirty = c.mode != s.mode || c.brightness != s.brightness || c.threshold != s.threshold || c.period != s.period;
    snprintk(out,size,"LED_CAPS=1\r\nLED_MODE=%s\r\nLED_BRIGHTNESS=%u\r\nLED_LOW_PCT=%u\r\nLED_PERIOD_MS=%u\r\nLED_DIRTY=%d\r\n",
             c.mode==0?"AUTO":c.mode==1?"ON":"OFF",c.brightness,c.threshold,c.period,dirty);
}
static void update_led(struct k_work *work) {
    k_spinlock_key_t key = k_spin_lock(&lock); struct led_config c = config; int64_t until = identify_until; k_spin_unlock(&lock,key);
    int64_t now = k_uptime_get();
    bool connected = hub_any_keyboard_connected(); int percent = hub_lowest_battery();
    if (previous_threshold != c.threshold) low = false;
    previous_threshold = c.threshold;
    low = connected && percent >= 0 && (low ? percent < c.threshold+5 : percent <= c.threshold);
    bool on = c.mode==1 || (c.mode==0 && dongle_led_on((uint32_t)((now%c.period)*2000/c.period),connected,percent,low));
    // Identification remains visible even when the user configured off/zero brightness.
    bool identify = now < until;
    uint8_t mask = (hub_keyboard_connected(0) ? 1 : 0) |
                   (hub_keyboard_connected(1) ? 2 : 0);
    uint8_t locked = (hub_keyboard_motion(0)==MOTION_LOCKED ? 1 : 0) |
                     (hub_keyboard_motion(1)==MOTION_LOCKED ? 2 : 0);
    enum led_motion_mode motion_mode = led_motion_select(mask,locked);
    bool link_on;
    bool link_event = led_link_update(&link_state, (uint32_t)now, mask,
                        c.mode != 2 && c.brightness > 0, identify, &link_on);
    if (link_event) {
        on = link_on;
    }
    if (identify) on = now%250 < 125;
    uint32_t duty = identify ? 100 : c.brightness;
    /* Identify, connection edges and low-battery warning keep priority. */
    if (c.mode==0 && !identify && !link_event && !low && motion_mode!=LED_MOTION_NORMAL) {
        on = true;
        duty = c.brightness * led_motion_level(motion_mode,(uint32_t)now)/100;
    }
    pwm_set_dt(&led,led.period,on ? led.period*duty/100 : 0);
    k_work_reschedule(&led_work,K_MSEC(25));
}
static int dongle_led_init(void) {
    if (!pwm_is_ready_dt(&led)) return -ENODEV;
    int err = pwm_set_dt(&led,led.period,0); if (err) return err;
    k_work_init_delayable(&led_work,update_led); k_work_schedule(&led_work,K_NO_WAIT); return 0;
}
SYS_INIT(dongle_led_init,APPLICATION,96);
