/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include "hub.h"
#include "axis_descriptor.h"
#include "axis-state.h"
#include "usb-port-names.h"
// Preserve the deployed receiver's VID/PID; no Bluetooth HCI is exposed.
USBD_DEVICE_DEFINE(hub_usb,DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),0x5048,0xff11);
USBD_DESC_LANG_DEFINE(hub_lang);
USBD_DESC_MANUFACTURER_DEFINE(hub_mfr,"Team PHDesign");
USBD_DESC_PRODUCT_DEFINE(hub_product,"PH Lite Wireless Hub");
USBD_DESC_SERIAL_NUMBER_DEFINE(hub_sn);
USBD_DESC_CONFIG_DEFINE(hub_cfg_desc,"Two keyboards + BLE mouse + Studio");
USBD_CONFIGURATION_DEFINE(hub_cfg,0,50,&hub_cfg_desc);
static const struct device *const hid=DEVICE_DT_GET(DT_NODELABEL(hub_hid));
// ID1: modifier, reserved, 104-bit NKRO; ID2: six 16-bit consumer usages;
// ID3: eight buttons and four signed 16-bit relative axes.
static const uint8_t descriptor[]={
    0x05,1,0x09,6,0xa1,1,0x85,1,0x05,7,0x19,0xe0,0x29,0xe7,
    0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,0x75,8,0x95,1,0x81,1,
    0x05,8,0x19,1,0x29,5,0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,
    0x05,7,0x19,0,0x29,0x67,0x75,1,0x95,104,0x81,2,0xc0,
    0x05,0x0c,0x09,1,0xa1,1,0x85,2,0x15,0,0x26,0xff,0x0f,
    0x19,0,0x2a,0xff,0x0f,0x75,16,0x95,12,0x81,0,0xc0,
    0x05,1,0x09,2,0xa1,1,0x85,3,0x09,1,0xa1,0,
    0x05,9,0x19,1,0x29,8,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
    0x05,1,0x16,0,0x80,0x26,0xff,0x7f,0x75,16,0x95,3,
    0x09,0x30,0x09,0x31,0x09,0x38,0x81,6,
    0x05,0x0c,0x0a,0x38,2,0x95,1,0x81,6,0xc0,0xc0,
    IMU_AXIS_DESCRIPTOR(4), IMU_AXIS_DESCRIPTOR(5)
};
struct usb_report { uint8_t len,data[25]; };
K_MSGQ_DEFINE(usb_reports,sizeof(struct usb_report),64,4);
static atomic_t ready, axis_force;
static atomic_t axes[2], axis_time[2];
void hub_axis_event(unsigned slot,int16_t axis){if(slot<2){atomic_set(&axes[slot],axis);atomic_set(&axis_time[slot],k_uptime_get_32());}}
static void submit(uint8_t id,const uint8_t *data,unsigned len) {
    uint8_t transfer[28] __aligned(4);transfer[0]=id;memcpy(transfer+1,data,len);
    hid_device_submit_report(hid,len+1,transfer);
}

static uint8_t last[5][24];
static uint8_t button_state[3];
static struct k_mutex mouse_lock;
static const uint8_t lengths[]={15,24,9,2,2};
static void iface_ready(const struct device *dev,bool on){atomic_set(&ready,on);if(on)atomic_set(&axis_force,1);}
static int get_report(const struct device *dev,uint8_t type,uint8_t id,uint16_t len,uint8_t *buf) {
    if(type!=HID_REPORT_TYPE_INPUT || id<1 || id>5 || len<lengths[id-1])return -ENOTSUP;
    memcpy(buf,last[id-1],lengths[id-1]);return lengths[id-1];
}
static int set_report(const struct device *dev,uint8_t type,uint8_t id,uint16_t len,const uint8_t *buf) {
    // Keyboard indicator forwarding is not yet implemented in this prototype.
    return type==HID_REPORT_TYPE_OUTPUT && id==1 && len>=1 ? 0:-ENOTSUP;
}
static const struct hid_device_ops ops={.iface_ready=iface_ready,.get_report=get_report,.set_report=set_report};
int hub_usb_report(uint8_t id,const uint8_t *data,size_t len) {
    if(id<1 || id>5 || len!=lengths[id-1])return -EINVAL;
    struct usb_report r={.len=len+1};r.data[0]=id;memcpy(r.data+1,data,len);
    return k_msgq_put(&usb_reports,&r,K_MSEC(20));
}
static int16_t clamp_axis(int32_t n){return n>32767?32767:n<-32768?-32768:n;}
void hub_mouse_event(unsigned source,const struct mouse_event *e) {
    if(source>2)return;
    k_mutex_lock(&mouse_lock,K_FOREVER);
    button_state[source]=(button_state[source]&~e->button_mask)|(e->buttons&e->button_mask);
    uint8_t r[9]={button_state[0]|button_state[1]|button_state[2]};
    sys_put_le16(clamp_axis(e->x),r+1);sys_put_le16(clamp_axis(e->y),r+3);
    sys_put_le16(clamp_axis(e->wheel),r+5);sys_put_le16(clamp_axis(e->pan),r+7);
    hub_usb_report(3,r,sizeof(r));
    k_mutex_unlock(&mouse_lock);
}
void hub_mouse_release(unsigned source){struct mouse_event e={.button_mask=255};hub_mouse_event(source,&e);}
static void usb_thread(void) {
    struct usb_report r;uint32_t sent=0,last_axis_sent[2]={0};
    for(;;) {
        if(!k_msgq_get(&usb_reports,&r,K_MSEC(10))){
            memcpy(last[r.data[0]-1],r.data+1,r.len-1);
            if(atomic_get(&ready))submit(r.data[0],r.data+1,r.len-1);
            if(r.data[0]==3)memset(last[2]+1,0,8);
        }
        uint32_t now=k_uptime_get_32();
        if(atomic_get(&ready) && now-sent>=20){
            sent=now;bool force=atomic_set(&axis_force,0);
            for(unsigned i=0;i<2;i++){
                int16_t axis=hub_axis_fresh(now,(uint32_t)atomic_get(&axis_time[i]),(int16_t)atomic_get(&axes[i]));
                if(force || axis!=(int16_t)sys_get_le16(last[i+3]) || now-last_axis_sent[i]>=200){sys_put_le16((uint16_t)axis,last[i+3]);submit(i+4,last[i+3],2);last_axis_sent[i]=now;}
            }
        }
    }
}
K_THREAD_DEFINE(usb_output,2048,usb_thread,NULL,NULL,NULL,7,0,0);
int hub_usb_init(void) {
    k_mutex_init(&mouse_lock);
    int err=hid_device_register(hid,descriptor,sizeof(descriptor),&ops);if(err)return err;
    struct usbd_desc_node *descs[]={&hub_lang,&hub_mfr,&hub_product,&hub_sn};
    for(int i=0;i<ARRAY_SIZE(descs);i++){err=usbd_add_descriptor(&hub_usb,descs[i]);if(err)return err;}
    err=usbd_add_configuration(&hub_usb,USBD_SPEED_FS,&hub_cfg);if(err)return err;
    err=usbd_register_all_classes(&hub_usb,USBD_SPEED_FS,1,NULL);if(err)return err;
    usbd_device_set_code_triple(&hub_usb,USBD_SPEED_FS,USB_BCC_MISCELLANEOUS,2,1);
    err=usbd_init(&hub_usb);if(err)return err;
    hub_usb_name_ports();
    return usbd_enable(&hub_usb);
}
