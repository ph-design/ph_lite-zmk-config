"""Run the actual power manager on a host with mocked clock, workqueue and devices."""
from pathlib import Path
import tempfile,subprocess
assets=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='power-runtime-') as directory:
 root=Path(directory)
 for name in ['zephyr/kernel.h','zephyr/init.h','zephyr/settings/settings.h','zmk/motion_guard.h','zmk/usb.h','zmk/studio/core.h','zmk/rgb_underglow.h']:
  p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('/* mocked */\n')
 (root/'zmk/power.h').write_text((assets/'power.h').read_text().replace('"../../src/power_policy.h"','"'+(assets/'power_policy.h').as_posix()+'"'))
 prelude=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CONFIG_ZMK_RGB_UNDERGLOW 1
#define CONFIG_ZMK_BACKLIGHT 1
#define CONFIG_USB_DEVICE_STACK 1
#define CONFIG_ZMK_STUDIO 1
#define CONFIG_APPLICATION_INIT_PRIORITY 90
#define IS_ENABLED(x) (x)
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(x) _Static_assert(x,#x)
#define SYS_INIT(...)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)
struct k_spinlock {int unused;}; typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock *x){return 0;}
static void k_spin_unlock(struct k_spinlock *x,int key){}
struct k_work {void (*fn)(struct k_work *);bool pending;};
#define K_WORK_DEFINE(name,cb) struct k_work name={cb,false}
static int k_work_submit(struct k_work *work){work->pending=true;return 0;}
static int64_t mock_time;
static int64_t k_uptime_get(void){return mock_time;}
static bool usb,locked,studio,key_path;
static unsigned rgb_calls,backlight_calls,saves;
static bool zmk_usb_is_powered(void){return usb;}
static bool zmk_motion_guard_blocks_output(void){return locked;}
#define ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED 1
static int zmk_studio_core_get_lock_state(void){return studio?1:0;}
static int zmk_rgb_underglow_apply_state(void){assert(!key_path);rgb_calls++;return 0;}
int zmk_backlight_power_refresh(void){assert(!key_path);backlight_calls++;return 0;}
typedef int (*settings_read_cb)(void *,void *,size_t);
static uint8_t stored[64];static size_t stored_len;
static int settings_save_one(const char *name,const void *data,size_t len){assert(!key_path);memcpy(stored,data,len);stored_len=len;saves++;return 0;}
static int settings_delete(const char *name){stored_len=0;return 0;}
'''
 body=r'''
static void flush(void){for(int i=0;i<5;i++){if(update_work.pending){update_work.pending=false;update_work.fn(&update_work);}if(lights_work.pending){lights_work.pending=false;lights_work.fn(&lights_work);}}}
static void key(uint32_t position,bool down){key_path=true;zmk_power_note_key(position,down);key_path=false;}
static int read_saved(void *arg,void *data,size_t len){memcpy(data,stored,len);return len;}
int main(void){
 zmk_power_update();assert(rgb_calls==0);power_init();zmk_power_update();
 mock_time=30000;zmk_power_update();assert(zmk_power_rgb_off() && !zmk_power_idle());flush();assert(rgb_calls==1 && backlight_calls==1);
 key(4,true);assert(!zmk_power_rgb_off() && held_count==1);assert(rgb_calls==1);flush();assert(rgb_calls==2);
 key(4,true);assert(held_count==1);mock_time+=7200000;zmk_power_update();assert(!zmk_power_idle() && !zmk_power_rgb_off());
 key(4,false);assert(held_count==0);mock_time+=120000;zmk_power_update();assert(zmk_power_idle());flush();
 struct zmk_power_config c;zmk_power_get_config(&c);c.deep_sleep_ms=1800000;assert(!zmk_power_set_config(&c));flush();mock_time+=1800000;zmk_power_update();assert(zmk_power_deep_sleep());
 studio=true;zmk_power_update();assert(!zmk_power_deep_sleep());studio=false;usb=true;zmk_power_update();assert(!zmk_power_idle() && !zmk_power_rgb_off() && !zmk_power_deep_sleep());
 locked=true;zmk_power_update();assert(zmk_power_rgb_off());c.off_when_locked=false;zmk_power_set_config(&c);flush();assert(!zmk_power_rgb_off());
 usb=false;locked=false;key(5,true);key(5,false);flush();assert(!zmk_power_deep_sleep());
 assert(saves==0);assert(!zmk_power_save_config() && saves==1);struct power_blob saved;memcpy(&saved,stored,sizeof(saved));
 c.rgb_off_ms=1000;zmk_power_set_config(&c);flush();assert(!power_settings_load("v1",stored_len,read_saved,NULL));flush();zmk_power_get_config(&c);assert(c.rgb_off_ms==30000 && c.deep_sleep_ms==1800000);
 saved.version=2;memcpy(stored,&saved,sizeof(saved));assert(power_settings_load("v1",stored_len,read_saved,NULL)==-EINVAL);
 c.rgb_off_ms=1234;assert(zmk_power_set_config(&c)==-EINVAL);zmk_power_get_config(&c);assert(c.rgb_off_ms==30000);
 assert(saves==1);assert(!zmk_power_reset_config());flush();zmk_power_get_config(&c);assert(c.deep_sleep_ms==0 && c.off_when_locked && stored_len==0);
 puts("PASS: actual power clocks, held/released keys, deferred lights, USB/Studio gating, validated save/load/reset; no flash writes from idle or input");
 return 0;
}
'''
 harness=root/'test.c';harness.write_text(prelude+'\n#include "'+(assets/'power.c').as_posix()+'"\n'+body)
 subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined','-I',str(root),str(harness),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
