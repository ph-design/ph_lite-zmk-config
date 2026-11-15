"""Run the actual game controller and its output work with a simulated clock."""
from pathlib import Path
import tempfile,subprocess
assets=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix="imu-game-") as directory:
 r=Path(directory)
 for name in ["zephyr/kernel.h","zephyr/settings/settings.h","zephyr/drivers/sensor.h","zmk/motion_guard.h","zmk/endpoints.h","zmk/power.h"]:
  p=r/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text("/* mock */\n")
 (r/"zmk/imu_game.h").write_bytes((assets/"imu_game.h").read_bytes())
 source=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#define CONFIG_ZMK_USB 1
#define CONFIG_ZMK_2G4 1
#define IS_ENABLED(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define BIT(i) (1U<<(i))
#define K_MSEC(x) (x)
#define K_THREAD_DEFINE(...)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)
typedef int32_t atomic_t;typedef int32_t atomic_val_t;
static int32_t atomic_get(atomic_t*p){return *p;}static int32_t atomic_set(atomic_t*p,int32_t v){int32_t old=*p;*p=v;return old;}static void atomic_or(atomic_t*p,int32_t v){*p|=v;}
struct k_spinlock{int x;};typedef int k_spinlock_key_t;
static int k_spin_lock(struct k_spinlock*l){return 0;}static void k_spin_unlock(struct k_spinlock*l,int k){}
struct k_work{void(*fn)(struct k_work*);};
#define K_WORK_DEFINE(name,fn) struct k_work name={fn}
static void k_work_submit(struct k_work*w){w->fn(w);}
static int32_t now;static uint32_t k_uptime_get_32(void){return now;}
static jmp_buf finish;static int steps,limit;static void(*hook)(void);
static void k_sleep(int ms){now+=ms;if(hook)hook();if(++steps>=limit)longjmp(finish,1);}
struct sensor_value{int32_t val1,val2;};static struct sensor_value sample[3];static bool sensor_ok=true;
static int zmk_motion_guard_read_accel(struct sensor_value*out){if(!sensor_ok)return -EIO;memcpy(out,sample,sizeof(sample));return 0;}
enum zmk_transport{ZMK_TRANSPORT_USB,ZMK_TRANSPORT_BLE,ZMK_TRANSPORT_2G4};
struct endpoint{enum zmk_transport transport;};static enum zmk_transport transport;
static struct endpoint zmk_endpoints_selected(void){return (struct endpoint){transport};}
static unsigned activity;static void zmk_power_note_activity(void){activity++;}
static int16_t usb,radio;static unsigned usb_count,radio_count;static bool usb_busy;
static int zmk_usb_hid_send_axis(int16_t value){if(usb_busy)return -EAGAIN;usb=value;usb_count++;return 0;}
static int zmk_2g4_send_axis(int16_t value){radio=value;radio_count++;return 0;}
typedef int(*settings_read_cb)(void*,void*,size_t);
static unsigned saves;static uint8_t blob[128];static size_t blob_len;
static int settings_save_one(const char*n,const void*p,size_t len){memcpy(blob,p,len);blob_len=len;saves++;return 0;}
static int settings_delete(const char*n){blob_len=0;return 0;}
'''
 # Exported prototypes must agree with the real definitions above.
 source=source.replace("static int zmk_usb_hid_send_axis","int zmk_usb_hid_send_axis").replace("static int zmk_2g4_send_axis","int zmk_2g4_send_axis")
 source+='\n#include "'+(assets/"imu_game.c").as_posix()+'"\n'
 source+=r'''
static void run(int n){steps=0;limit=n;if(!setjmp(finish))game_thread(0,0,0);}
static void flat(void){sample[0]=(struct sensor_value){0,0};sample[1]=(struct sensor_value){0,0};sample[2]=(struct sensor_value){9,806650};}
static void tilt(void){sample[0]=(struct sensor_value){-6,-935720};sample[1]=(struct sensor_value){0,0};sample[2]=(struct sensor_value){6,935720};}
static void stop_hook(void){if(steps==2)zmk_imu_game_mode(false);}
static void switch_hook(void){if(steps==2)transport=ZMK_TRANSPORT_2G4;}
static void fault_hook(void){if(steps==2)sensor_ok=false;}
static void retry_hook(void){if(steps==2){usb_busy=true;zmk_imu_game_mode(false);}if(steps==4)assert(atomic_get(&pending_output));if(steps==6)usb_busy=false;}
int main(void){
 assert(imu_axis(0,45,2,false)==0 && imu_axis(1.9f,45,2,false)==0);
 assert(imu_axis(45,45,2,false)==32767 && imu_axis(-90,45,2,false)==-32767 && imu_axis(-45,45,2,true)==32767);
 assert(fabsf(imu_difference(-179,179)-2)<.01f);
 const struct imu_game_config defaults=IMU_GAME_DEFAULTS;assert(!zmk_imu_game_set_config(&defaults));
 struct imu_game_config bad=defaults;bad.range=2;assert(zmk_imu_game_set_config(&bad)==-EINVAL);bad=defaults;bad.deadzone=45;assert(zmk_imu_game_set_config(&bad)==-EINVAL);bad=defaults;bad.center_pitch=9001;assert(zmk_imu_game_set_config(&bad)==-EINVAL);
 flat();zmk_imu_game_view(true);run(2);assert(state.valid && !state.active && usb_count==0);
 tilt();assert(!zmk_imu_game_mode(true));run(5);assert(usb>32000 && activity>0 && saves==0);
 hook=stop_hook;run(7);hook=0;assert(!zmk_imu_game_active() && usb==0);
 assert(!zmk_imu_game_mode(true));run(160);assert(!zmk_imu_game_active() && usb==0); /* lease expires */
 assert(!zmk_imu_game_mode(true));hook=switch_hook;run(7);hook=0;assert(usb==0 && radio>32000);
 hook=fault_hook;run(7);hook=0;assert(!zmk_imu_game_active() && radio==0);sensor_ok=true;
 transport=ZMK_TRANSPORT_BLE;assert(zmk_imu_game_mode(true)==-ENOTSUP);
 transport=ZMK_TRANSPORT_USB;assert(!zmk_imu_game_mode(true));hook=retry_hook;run(10);hook=0;assert(usb==0 && !atomic_get(&pending_output));
 assert(!zmk_imu_game_save() && saves==1);zmk_imu_game_reset();assert(!zmk_imu_game_active());
 puts("PASS: actual game controller deadzone/range/invert/wrap, bounds, idle no output, explicit mode, lease expiry, transport change, sensor failure, output retry and explicit persistence");return 0;
}
'''
 (r/"test.c").write_text(source)
 subprocess.run(["gcc","-std=c11","-Wall","-Wextra","-Wno-unused-parameter","-Wno-unused-function","-fsanitize=address,undefined","-I",str(r),str(r/"test.c"),"-lm","-o",str(r/"test")],check=True)
 subprocess.run([str(r/"test")],check=True)
