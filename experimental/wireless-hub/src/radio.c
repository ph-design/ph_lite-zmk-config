/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#include <esb.h>
#include "hub.h"
#include "studio-radio.h"
#include "hub-peer.h"
#include "input-merge.h"
#include "diagnostics.h"
#include <string.h>
K_MSGQ_DEFINE(radio_packets,sizeof(struct esb_payload),32,4);
struct keyboard_status { atomic_t rx,fail,drop,connected,last_packet,battery,last_battery,motion,last_motion; };
static struct keyboard_status status[HUB_PEERS];
static struct input_merge input;
static atomic_t release_mask;
K_MUTEX_DEFINE(radio_config_lock);
static bool started;
static bool paused_by_ble;
int hub_radio_error;
int hub_radio_pause(bool pause) {
    k_mutex_lock(&radio_config_lock,K_FOREVER);
    hub_diagnostic_stage(pause?31:32);
    int err=pause ? esb_stop_rx():esb_start_rx();
    if(!err)paused_by_ble=pause;
    k_mutex_unlock(&radio_config_lock);return err;
}
static void esb_event(const struct esb_evt *evt) {
    if(evt->evt_id==ESB_EVENT_RX_RECEIVED) {
        struct esb_payload p;
        while(!esb_read_rx_payload(&p))if(k_msgq_put(&radio_packets,&p,K_NO_WAIT) && p.pipe<HUB_PEERS)atomic_inc(&status[p.pipe].drop);
    }
}
bool hub_keyboard_connected(unsigned i){return i<HUB_PEERS && atomic_get(&status[i].connected);}
enum motion_status hub_keyboard_motion(unsigned i) {
    if(i>=HUB_PEERS)return MOTION_UNKNOWN;
    return motion_status_fresh(atomic_get(&status[i].motion),
        k_uptime_get_32()-(uint32_t)atomic_get(&status[i].last_motion),hub_keyboard_connected(i));
}
int hub_keyboard_battery(unsigned i) {
    return hub_keyboard_connected(i) && k_uptime_get_32()-(uint32_t)atomic_get(&status[i].last_battery)<2000?atomic_get(&status[i].battery):-1;
}
void hub_keyboard_stats(unsigned i,uint32_t *rx,uint32_t *fail,uint32_t *drop) {
    if(i>=HUB_PEERS)return;
    if(rx)*rx=atomic_get(&status[i].rx);
    if(fail)*fail=atomic_get(&status[i].fail);
    if(drop)*drop=atomic_get(&status[i].drop);
}
bool hub_any_keyboard_connected(void){return hub_keyboard_connected(0)||hub_keyboard_connected(1);}
int hub_lowest_battery(void){int a=hub_keyboard_battery(0),b=hub_keyboard_battery(1);return a<0?b:b<0?a:MIN(a,b);}
bool zmk_2g4_dongle_kb_connected(void){return hub_keyboard_connected(hub_peer_target());}
int zmk_2g4_dongle_battery_percent(void){return hub_keyboard_battery(hub_peer_target());}
void zmk_2g4_dongle_rx_stats(uint32_t *rx,uint32_t *fail){hub_keyboard_stats(hub_peer_target(),rx,fail,NULL);}
void hub_radio_release(unsigned i){if(i<HUB_PEERS)atomic_or(&release_mask,1u<<i);}
int zmk_esb_write_payload(const struct esb_payload *p) {
    // Only the selected peer has an active Studio producer; pipe routes its ACK.
    k_mutex_lock(&radio_config_lock,K_FOREVER);
    int err=paused_by_ble?-EAGAIN:esb_write_payload(p);
    k_mutex_unlock(&radio_config_lock);
    return err;
}
int hub_radio_reconfigure(void) {
    k_mutex_lock(&radio_config_lock,K_FOREVER);int err=0;
    hub_diagnostic_stage(20);
    if(!started) {
        struct esb_config cfg=ESB_DEFAULT_CONFIG;
        cfg.protocol=ESB_PROTOCOL_ESB_DPL;cfg.mode=ESB_MODE_PRX;
        cfg.bitrate=ESB_BITRATE_2MBPS;cfg.crc=ESB_CRC_16BIT;
        cfg.selective_auto_ack=false;cfg.event_handler=esb_event;
        err=esb_init(&cfg);if(err)goto done;
        started=true;
    } else if(!esb_is_idle()) {
        err=esb_stop_rx();if(err && err!=-EALREADY)goto done;
        int64_t until=k_uptime_get()+100;
        while(!esb_is_idle() && k_uptime_get()<until)k_msleep(1);
        if(!esb_is_idle()){err=-ETIMEDOUT;goto done;}
    }
    esb_flush_tx();esb_flush_rx();k_msgq_purge(&radio_packets);
    struct zmk_2g4_addr addr[2];hub_peer_addr(0,&addr[0]);hub_peer_addr(1,&addr[1]);
    err=esb_set_base_address_0(addr[0].base);if(err)goto done;
    err=esb_set_base_address_1(addr[1].base);if(err)goto done;
    uint8_t prefixes[]={addr[0].prefix,addr[1].prefix};
    err=esb_set_prefixes(prefixes,hub_peer_enabled(1)?2:1);if(err)goto done;
    err=esb_set_rf_channel(addr[0].rf_channel);if(err)goto done;
    esb_set_tx_power(4);
    err=zmk_2g4_crypto_reserve_session_id();if(err)goto done;
    zmk_2g4_crypto_session_start();studio_radio_started();
    err=paused_by_ble?0:esb_start_rx();
done:hub_radio_error=err;k_mutex_unlock(&radio_config_lock);return err;
}
int zmk_2g4_dongle_start(void){return hub_radio_reconfigure();}
static void send_keyboard(void){uint8_t out[15];merge_keyboard(&input,out);hub_usb_report(1,out,15);}
static void send_consumer(void){uint8_t out[24];merge_consumer(&input,out);hub_usb_report(2,out,24);}
static void release(unsigned slot) {
    atomic_clear(&status[slot].connected);atomic_set(&status[slot].battery,-1);
    atomic_set(&status[slot].motion,MOTION_UNKNOWN);
    memset(input.keyboard[slot],0,15);memset(input.consumer[slot],0,12);
    send_keyboard();send_consumer();hub_mouse_release(slot);
}
static void packet(struct esb_payload *p) {
    unsigned slot=p->pipe;if(slot>=HUB_PEERS || !hub_peer_enabled(slot))return;
    struct keyboard_status *s=&status[slot];atomic_inc(&s->rx);
    int n=hub_peer_decrypt(slot,p->data,p->length);
    if(n<1){if(n!=-EACCES)atomic_inc(&s->fail);return;}
    atomic_set(&s->connected,1);atomic_set(&s->last_packet,k_uptime_get_32());
    switch(p->data[0]) {
    case 0x05:atomic_set(&s->motion,MOTION_UNKNOWN);break;
    case 0x40:studio_radio_received_from(slot,p->data,n);break;
    case 1:if(n==16){memcpy(input.keyboard[slot],p->data+1,15);send_keyboard();}break;
    case 2:if(n==13){memcpy(input.consumer[slot],p->data+1,12);send_consumer();}break;
    case 3:if(n==10){
        struct mouse_event e={.buttons=p->data[1],.button_mask=255,
            .x=(int16_t)sys_get_le16(p->data+2),.y=(int16_t)sys_get_le16(p->data+4),
            .wheel=(int16_t)sys_get_le16(p->data+6),.pan=(int16_t)sys_get_le16(p->data+8)};
        hub_mouse_event(slot,&e);
        }break;
    case 0xfe:atomic_set(&s->battery,n>=2 && p->data[1]<=100?p->data[1]:-1);
        atomic_set(&s->last_battery,k_uptime_get_32());
        atomic_set(&s->last_motion,k_uptime_get_32());
        atomic_set(&s->motion,motion_status_decode(p->data,n));break;
    }
}
void hub_radio_run(void) {
    struct esb_payload p;
    for(int i=0;i<HUB_PEERS;i++)atomic_set(&status[i].battery,-1);
    for(;;) {
        atomic_val_t pending=atomic_set(&release_mask,0);
        for(int i=0;i<HUB_PEERS;i++)if(pending&(1u<<i))release(i);
        if(!k_msgq_get(&radio_packets,&p,K_MSEC(20)))packet(&p);
        for(int i=0;i<HUB_PEERS;i++)if(hub_keyboard_connected(i) && k_uptime_get_32()-(uint32_t)atomic_get(&status[i].last_packet)>500)release(i);
    }
}
