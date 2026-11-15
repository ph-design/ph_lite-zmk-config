"""Exercise the actual deferred tap controller with hardware and clock mocks."""
from pathlib import Path
import tempfile, subprocess
assets = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='tap-runtime-') as directory:
    root = Path(directory)
    headers = ['zephyr/kernel.h','zephyr/init.h','zephyr/settings/settings.h','zephyr/device.h','zephyr/drivers/sensor.h','zephyr/drivers/sensor/lis2dh.h','zephyr/sys/byteorder.h','zmk/motion.h','zmk/imu_game.h','zmk/motion_guard.h','zmk/power.h','zmk/keymap.h','zmk/behavior.h','zmk/virtual_key_position.h','zmk/events/position_state_changed.h']
    for name in headers:
        p = root / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('/* mocked */\n')
    (root/'zmk/tap.h').write_text((assets/'tap.h').read_text())
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define CONFIG_ZMK_SPLIT 0
#define CONFIG_ZMK_MOTION_TAP_SIDE_INVERT 0
#define IS_ENABLED(x) (x)
#define BIT(x) (1U<<(x))
#define CLAMP(v,l,h) ((v)<(l)?(l):((v)>(h)?(h):(v)))
#define ARG_UNUSED(x) (void)(x)
#define K_NO_WAIT 0
#define K_MSEC(x) (x)
#define K_SECONDS(x) ((x)*1000)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)
#define ZMK_VIRTUAL_KEY_POSITION_MOTION(x) (100+(x))
static int64_t now;
static int64_t k_uptime_get(void){return now;}
static uint32_t k_uptime_get_32(void){return now;}
typedef int32_t atomic_t; typedef int32_t atomic_val_t;
static int32_t atomic_get(atomic_t*p){return *p;}
static void atomic_set(atomic_t*p,int32_t n){*p=n;}
static void atomic_inc(atomic_t*p){(*p)++;}
struct k_spinlock{int dummy;};typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock*p){return 0;} static void k_spin_unlock(struct k_spinlock*p,int k){}
struct k_work{int dummy;};struct k_work_delayable{int64_t due;bool pending;};
#define K_WORK_DELAYABLE_DEFINE(name,fn) struct k_work_delayable name
static void k_work_schedule(struct k_work_delayable*w,int delay){if(!w->pending){w->pending=true;w->due=now+delay;}}
static void k_work_reschedule(struct k_work_delayable*w,int delay){w->pending=true;w->due=now+delay;}
static void k_work_cancel_delayable(struct k_work_delayable*w){w->pending=false;}
struct queue{unsigned size,head,tail,used;uint8_t data[8][64];};
#define K_MSGQ_DEFINE(name,sz,n,align) struct queue name={.size=sz}
static int k_msgq_put(struct queue*q,const void*data,int wait){if(q->used==8)return -1;memcpy(q->data[q->tail],data,q->size);q->tail=(q->tail+1)%8;q->used++;return 0;}
static int k_msgq_get(struct queue*q,void*out,int wait){if(!q->used)return -1;memcpy(out,q->data[q->head],q->size);q->head=(q->head+1)%8;q->used--;return 0;}
static unsigned k_msgq_num_used_get(struct queue*q){return q->used;}
static void k_msgq_purge(struct queue*q){q->head=q->tail=q->used=0;}
struct device{int dummy;};struct sensor_trigger{int type,chan;};struct sensor_value{int32_t val1,val2;};
#define SENSOR_TRIG_TAP 1
#define SENSOR_TRIG_DOUBLE_TAP 2
#define SENSOR_CHAN_ACCEL_XYZ 3
#define SENSOR_CHAN_LIS2DH_CLICK_SRC 4
typedef void (*handler)(const struct device*,const struct sensor_trigger*);
static handler single_handler,double_handler;static uint8_t mock_src;
static int sensor_trigger_set(const struct device*d,const struct sensor_trigger*t,handler h){if(t->type==1)single_handler=h;else double_handler=h;return 0;}
static int sensor_channel_get(const struct device*d,int chan,struct sensor_value*out){out->val1=mock_src;out->val2=0;return 0;}
struct zmk_behavior_binding{const char*behavior_dev;};
struct zmk_motion_tap_config{bool enabled;uint32_t threshold,time_limit_ms,latency_ms,window_ms;struct zmk_behavior_binding left_single,left_double,right_single,right_double;uint32_t layer_mask;};
static struct zmk_motion_tap_config current;
static int zmk_motion_get_tap_config(struct zmk_motion_tap_config*out){*out=current;return 0;}
static bool zmk_imu_game_active(void){return false;}
static bool blocked,typing;static uint32_t guard_gen;
static bool zmk_motion_guard_blocks_output(void){return blocked;}
static uint32_t zmk_motion_guard_generation(void){return guard_gen;}
static bool zmk_power_input_recent(uint32_t ms){return typing;}
static uint32_t zmk_keymap_layer_state(void){return 1;}
static void zmk_power_note_activity(void){}
struct zmk_behavior_binding_event{uint32_t position;int64_t timestamp;};
static unsigned invoked;static const char*last_binding;
static void zmk_behavior_invoke_binding(const struct zmk_behavior_binding*b,struct zmk_behavior_binding_event e,bool down){if(down){invoked++;last_binding=b->behavior_dev;assert(e.position==100);}}
static void sys_put_le16(uint16_t v,uint8_t*p){p[0]=v;p[1]=v>>8;}
typedef int (*settings_read_cb)(void*,void*,size_t);
static uint8_t saved[32];static size_t saved_len;
static int settings_save_one(const char*n,const void*b,size_t len){memcpy(saved,b,len);saved_len=len;return 0;}
static int settings_delete(const char*n){saved_len=0;return 0;}
'''
    source += '\n#include "' + (assets/'tap.c').as_posix() + '"\n'
    source += r'''
int zmk_tap_sensor_configure(const struct device*d,uint8_t axes,bool dbl){assert(axes<=7);return 0;}
static const struct device dev;
static void reset(bool single,bool dbl){now=1000;invoked=0;blocked=typing=false;guard_gen++;current=(struct zmk_motion_tap_config){.enabled=true,.latency_ms=80,.window_ms=240,.left_single={single?"single":NULL},.left_double={dbl?"double":NULL}};options=(struct zmk_tap_options){7,120,false};atomic_set(&test_until,0);zmk_tap_rearm(&dev,&current);}
static void pulse(uint8_t src,int64_t time){now=time;mock_src=src;zmk_tap_driver_gpio();zmk_tap_driver_irq(src);irq_handler(&dev,NULL);now+=12;raw_work_cb(NULL);}
int main(void){
 reset(true,true);assert(!single_handler && double_handler && diagnostics.engine==2);pulse(0x60,1100);assert(invoked==1 && !strcmp(last_binding,"double"));
 reset(true,false);assert(!single_handler && double_handler);pulse(0x58,1100);pulse(0x50,1250);assert(invoked==0); /* saved single bindings inert */
 reset(true,true);pulse(0x58,1100);pulse(0x50,1250);assert(invoked==0); /* no software pairing */
 reset(false,true);pulse(0x60,1100);pulse(0x60,1120);assert(invoked==1); /* duplicate rejection */
 reset(false,true);blocked=true;guard_gen++;pulse(0x60,1100);blocked=false;pulse(0x60,1250);assert(invoked==1);
 reset(false,true);typing=true;pulse(0x60,1100);typing=false;assert(invoked==0);pulse(0x60,1500);assert(invoked==1);
 reset(true,true);assert(!zmk_tap_set_test(true));assert(!single_handler && double_handler && diagnostics.engine==2);assert(diagnostics.actions==0);now=60995;mock_src=0x62;irq_handler(&dev,NULL);now=61010;raw_work_cb(NULL);assert(invoked==0); /* queued test event cannot execute after expiry */
 test_expiry_cb(NULL);assert(!zmk_tap_test_active());pulse(0x62,62000);assert(invoked==1 && !strcmp(last_binding,"double"));
 reset(false,true);for(int i=0;i<9;i++){mock_src=0x60;irq_handler(&dev,NULL);}raw_work_cb(NULL);assert(invoked==0 && diagnostics.drops>0);
 reset(false,true);struct sensor_value xyz[3]={{0,0},{0,0},{9,806650}};for(int i=0;i<40;i++){now++;zmk_tap_sample(xyz);}zmk_tap_driver_irq(0x62);struct zmk_tap_capture cap;zmk_tap_get_capture(&cap);assert(cap.count==32 && cap.frames[6]==0xe8 && cap.frames[7]==3);
 puts("PASS: hardware double-only including test mode, inert legacy singles, no software pairing, debounce, lock generations, typing gate, no-action test expiry, overflow rejection and bounded capture");return 0;
}
'''
    (root/'test.c').write_text(source)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-function','-fsanitize=address,undefined','-I',str(root),str(root/'test.c'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
