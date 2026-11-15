/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/sensor.h>
#include <zmk/imu_game.h>
#include <zmk/motion_guard.h>
#include <zmk/endpoints.h>
#include <zmk/power.h>
#include "imu_math.h"
static struct imu_game_config config=IMU_GAME_DEFAULTS;
static struct imu_game_state state;
static struct k_spinlock lock;
static atomic_t lease, viewing;
void zmk_imu_game_view(bool enabled){atomic_set(&viewing,enabled);}
static uint32_t revision;
bool zmk_imu_game_active(void) { uint32_t until=atomic_get(&lease);return until && (int32_t)(until-k_uptime_get_32())>0; }
void zmk_imu_game_get_config(struct imu_game_config *out) { k_spinlock_key_t key=k_spin_lock(&lock);*out=config;k_spin_unlock(&lock,key); }
int zmk_imu_game_set_config(const struct imu_game_config *c) {
    if(c->source>1 || c->range<5 || c->range>90 || c->deadzone>10 || c->deadzone>=c->range || c->smoothing>200 || c->flip>7 || c->center_roll<-18000 || c->center_roll>18000 || c->center_pitch<-9000 || c->center_pitch>9000)return -EINVAL;
    k_spinlock_key_t key=k_spin_lock(&lock);config=*c;revision++;k_spin_unlock(&lock,key);return 0;
}
static bool supported(void) { enum zmk_transport transport=zmk_endpoints_selected().transport;return transport==ZMK_TRANSPORT_USB
#if IS_ENABLED(CONFIG_ZMK_2G4)
 || transport==ZMK_TRANSPORT_2G4
#endif
 ; }
int zmk_imu_game_mode(bool enabled) {
    if(enabled) { struct sensor_value xyz[3];if(!supported())return -ENOTSUP;if(zmk_motion_guard_read_accel(xyz))return -EIO; }
    atomic_set(&lease,enabled?(atomic_val_t)(k_uptime_get_32()+3000):0);return 0;
}
void zmk_imu_game_state(struct imu_game_state *out) { k_spinlock_key_t key=k_spin_lock(&lock);*out=state;k_spin_unlock(&lock,key);out->active=zmk_imu_game_active(); }
/* Radio encryption + send stays on the same system workqueue as keys and
 * keepalives; no new producer can reorder their encrypted sequence numbers. */
static atomic_t pending_output, usb_axis, radio_axis;
static void output_cb(struct k_work *work) {
    ARG_UNUSED(work);atomic_val_t pending=atomic_set(&pending_output,0);
#if IS_ENABLED(CONFIG_ZMK_USB)
    if((pending&1) && zmk_usb_hid_send_axis((int16_t)atomic_get(&usb_axis)))atomic_or(&pending_output,1);
#endif
#if IS_ENABLED(CONFIG_ZMK_2G4)
    if((pending&2) && zmk_2g4_send_axis((int16_t)atomic_get(&radio_axis)))atomic_or(&pending_output,2);
#endif
}
static K_WORK_DEFINE(output_work,output_cb);
static int output(int16_t axis,enum zmk_transport transport) {
#if IS_ENABLED(CONFIG_ZMK_USB)
    if(transport==ZMK_TRANSPORT_USB){atomic_set(&usb_axis,axis);atomic_or(&pending_output,1);k_work_submit(&output_work);return 0;}
#endif
#if IS_ENABLED(CONFIG_ZMK_2G4)
    if(transport==ZMK_TRANSPORT_2G4){atomic_set(&radio_axis,axis);atomic_or(&pending_output,2);k_work_submit(&output_work);return 0;}
#endif
    return -ENOTSUP;
}
static void activity_cb(struct k_work *work){ARG_UNUSED(work);zmk_power_note_activity();}
static K_WORK_DEFINE(activity_work,activity_cb);
static void game_thread(void *a,void *b,void *c) {
    ARG_UNUSED(a);ARG_UNUSED(b);ARG_UNUSED(c);
    float filtered[3]={0};bool have=false,was_active=false;
    uint32_t seen=0;enum zmk_transport last=ZMK_TRANSPORT_USB;bool neutral=false;enum zmk_transport old=ZMK_TRANSPORT_USB;
    for(;;) {
        if(atomic_get(&pending_output))k_work_submit(&output_work);
        if(was_active && !zmk_imu_game_active()){neutral=true;old=last;was_active=false;}
        if(neutral && output(0,old)==0)neutral=false;
        if(!zmk_imu_game_active() && !atomic_get(&viewing)){have=false;k_sleep(K_MSEC(50));continue;}
        struct imu_game_config cfg;uint32_t rev;
        k_spinlock_key_t key=k_spin_lock(&lock);cfg=config;rev=revision;k_spin_unlock(&lock,key);
        struct imu_game_state next={.supported=supported(),.roll=state.roll,.pitch=state.pitch};struct sensor_value xyz[3];
        if(!zmk_motion_guard_read_accel(xyz)) {
            int32_t raw[3];float norm=0;
            for(unsigned i=0;i<3;i++){int64_t micro=(int64_t)xyz[i].val1*1000000+xyz[i].val2;raw[i]=micro*1000/9806650;norm+=(float)raw[i]*raw[i];}
            next.x=raw[0];next.y=raw[1];next.z=raw[2];next.time=k_uptime_get_32();
            next.valid=norm>=700.0f*700 && norm<=1300.0f*1300;
            float mapped[3]={raw[cfg.swap?1:0],raw[cfg.swap?0:1],raw[2]};
            if(rev!=seen){have=false;seen=rev;}
            if(next.valid){for(unsigned i=0;i<3;i++){if(cfg.flip&BIT(i))mapped[i]=-mapped[i];filtered[i]=!have?mapped[i]:filtered[i]+(mapped[i]-filtered[i])*(20.0f/(20+cfg.smoothing));}have=true;float roll,pitch;imu_angles(filtered,&roll,&pitch);next.roll=roll*100;next.pitch=pitch*100;
                next.axis=imu_axis(imu_difference(cfg.source?pitch:roll,(cfg.source?cfg.center_pitch:cfg.center_roll)/100.0f),cfg.range,cfg.deadzone,cfg.invert);
            }
        } else { atomic_set(&lease,0);have=false; }
        enum zmk_transport transport=zmk_endpoints_selected().transport;
        if(!next.supported)atomic_set(&lease,0);
        next.active=zmk_imu_game_active();
        if(was_active && (!next.active || transport!=last)){neutral=true;old=last;if(output(0,old)==0)neutral=false;}
        if(next.active){output(next.valid?next.axis:0,transport);k_work_submit(&activity_work);}
        else next.axis=0;
        key=k_spin_lock(&lock);state=next;k_spin_unlock(&lock,key);
        last=transport;was_active=next.active;k_sleep(K_MSEC(20));
    }
}
K_THREAD_DEFINE(imu_game,2048,game_thread,NULL,NULL,NULL,9,0,0);
struct game_blob {uint32_t version,source,range,deadzone,smoothing,flags;int32_t center_roll,center_pitch;};
int zmk_imu_game_save(void){struct imu_game_config c;zmk_imu_game_get_config(&c);struct game_blob b={1,c.source,c.range,c.deadzone,c.smoothing,(c.invert?1:0)|(c.swap?2:0)|(c.flip<<2),c.center_roll,c.center_pitch};return settings_save_one("imu_game/v1",&b,sizeof(b));}
int zmk_imu_game_reset(void){atomic_set(&lease,0);const struct imu_game_config c=IMU_GAME_DEFAULTS;zmk_imu_game_set_config(&c);int rc=settings_delete("imu_game/v1");return rc==-ENOENT?0:rc;}
static int load(const char *name,size_t len,settings_read_cb read_cb,void *arg){struct game_blob b;if(strcmp(name,"v1"))return -ENOENT;if(len!=sizeof(b)||read_cb(arg,&b,sizeof(b))!=sizeof(b)||b.version!=1||b.flags>31)return -EINVAL;struct imu_game_config c={b.source,b.range,b.deadzone,b.smoothing,(b.flags&1)!=0,(b.flags&2)!=0,b.flags>>2,b.center_roll,b.center_pitch};if(IMU_BOARD_FLIP && b.source==1 && b.range==45 && b.deadzone==2 && b.smoothing==60 && !b.flags && !b.center_roll && !b.center_pitch)c.flip=IMU_BOARD_FLIP;return zmk_imu_game_set_config(&c);}
SETTINGS_STATIC_HANDLER_DEFINE(imu_game_settings,"imu_game",NULL,load,NULL,NULL);
