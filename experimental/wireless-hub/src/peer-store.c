/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <string.h>
#include "hub-peer.h"
#include "peer-crypto.h"
struct peer_record { uint8_t version,enabled,paired,key[16];struct zmk_2g4_addr addr; } __packed;
struct peer_state { struct peer_record record;struct peer_crypto crypto; };
static struct peer_state peers[HUB_PEERS];
static struct peer_record loaded[HUB_PEERS];
static bool have_record[HUB_PEERS],reserved;
static uint32_t epoch;
static atomic_t target;
static uint8_t factory_key[16];
K_MUTEX_DEFINE(peer_lock);
static bool valid_addr(const struct zmk_2g4_addr *a) {
    return a->rf_channel<=100 && a->rf_channel!=2 && a->rf_channel!=26 && a->rf_channel!=80;
}
void zmk_2g4_addr_get_default(struct zmk_2g4_addr *a) {
    *a=(struct zmk_2g4_addr){.base={0x50,0x48,0x44,0x53},.prefix=0x4e,.rf_channel=76};
}
static int initial(void) {
    if(hex2bin(CONFIG_ZMK_2G4_AES_KEY,32,factory_key,16)!=16)return -EINVAL;
    for(int i=0;i<HUB_PEERS;i++) {
        peers[i].record.version=1;memcpy(peers[i].record.key,factory_key,16);
        zmk_2g4_addr_get_default(&peers[i].record.addr);
    }
    peers[0].record.enabled=1;return 0;
}
SYS_INIT(initial,APPLICATION,CONFIG_APPLICATION_INIT_PRIORITY);
static int legacy_set(const char *name,size_t len,settings_read_cb read,void *arg) {
    if(!strcmp(name,"key")) {
        if(len!=16 || read(arg,peers[0].record.key,16)!=16)return -EINVAL;
        peers[0].record.paired=1;return 0;
    }
    if(!strcmp(name,"addr")) {
        if(len!=6 || read(arg,&peers[0].record.addr,6)!=6)return -EINVAL;
        return 0;
    }
    if(!strcmp(name,"tx_epoch"))return len==4 && read(arg,&epoch,4)==4?0:-EINVAL;
    return -ENOENT;
}
static int hub_set(const char *name,size_t len,settings_read_cb read,void *arg) {
    if(!strcmp(name,"target")) {
        uint8_t n;if(len!=1 || read(arg,&n,1)!=1 || n>=HUB_PEERS)return -EINVAL;
        atomic_set(&target,n);return 0;
    }
    unsigned slot;if(!strcmp(name,"peer0"))slot=0;else if(!strcmp(name,"peer1"))slot=1;else return -ENOENT;
    struct peer_record r;
    if(len!=sizeof(r) || read(arg,&r,sizeof(r))!=sizeof(r) || r.version!=1 || r.enabled>1 || r.paired>1 || !valid_addr(&r.addr))return -EINVAL;
    loaded[slot]=r;have_record[slot]=true;return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(legacy_radio,"2g4",NULL,legacy_set,NULL,NULL);
SETTINGS_STATIC_HANDLER_DEFINE(hub_radio,"hub",NULL,hub_set,NULL,NULL);
void hub_peer_load_finish(void) {
    for(int i=0;i<HUB_PEERS;i++)if(have_record[i])peers[i].record=loaded[i];
    // Never enable incompatible or duplicate peers from a malformed stored record.
    if(peers[1].record.enabled &&
       (peers[0].record.addr.rf_channel!=peers[1].record.addr.rf_channel ||
        !memcmp(peers[0].record.key,peers[1].record.key,16) ||
        !memcmp(&peers[0].record.addr,&peers[1].record.addr,5)))peers[1].record.enabled=0;
    for(int i=0;i<HUB_PEERS;i++)memcpy(peers[i].crypto.key,peers[i].record.key,16);
    if(!peers[atomic_get(&target)].record.enabled)atomic_set(&target,0);
}
unsigned hub_peer_target(void){return atomic_get(&target);}
bool hub_peer_enabled(unsigned i){if(i>=HUB_PEERS)return false;k_mutex_lock(&peer_lock,K_FOREVER);bool enabled=peers[i].record.enabled;k_mutex_unlock(&peer_lock);return enabled;}
bool hub_peer_paired(unsigned i){if(i>=HUB_PEERS)return false;k_mutex_lock(&peer_lock,K_FOREVER);bool paired=peers[i].record.paired;k_mutex_unlock(&peer_lock);return paired;}
void hub_peer_addr(unsigned i,struct zmk_2g4_addr *a){k_mutex_lock(&peer_lock,K_FOREVER);*a=peers[i<HUB_PEERS?i:0].record.addr;k_mutex_unlock(&peer_lock);}
uint32_t hub_peer_key_id(unsigned i){if(i>=HUB_PEERS)return 0;k_mutex_lock(&peer_lock,K_FOREVER);uint32_t id=crc32_ieee(peers[i].record.key,16);k_mutex_unlock(&peer_lock);return id;}
static int save_record(unsigned i,const struct peer_record *r) {
    char path[16];snprintk(path,sizeof(path),"hub/peer%u",i);
    int err=settings_save_one(path,r,sizeof(*r));if(err)return err;
    peers[i].record=*r;memcpy(peers[i].crypto.key,r->key,16);
    // Keep the TX epoch and counter: pairing with the same key must not reuse a nonce.
    peers[i].crypto.rx_initialized=false;
    hub_radio_release(i);return 0;
}
int hub_peer_pair(unsigned i,const uint8_t key[16],const struct zmk_2g4_addr *a) {
    if(i>=HUB_PEERS || !key || !a || !valid_addr(a))return -EINVAL;
    if(studio_radio_busy())return -EBUSY;
    k_mutex_lock(&peer_lock,K_FOREVER);
    unsigned other=1-i;int err=0;
    if(peers[other].record.enabled &&
       (a->rf_channel!=peers[other].record.addr.rf_channel ||
        !memcmp(key,peers[other].record.key,16) ||
        !memcmp(a,&peers[other].record.addr,5)))err=-EINVAL;
    else {
        struct peer_record r={.version=1,.enabled=1,.paired=1,.addr=*a};memcpy(r.key,key,16);
        err=save_record(i,&r);memset(r.key,0,16);
    }
    k_mutex_unlock(&peer_lock);return err;
}
int hub_peer_clear(unsigned i) {
    if(i>=HUB_PEERS)return -EINVAL;
    if(studio_radio_busy())return -EBUSY;
    k_mutex_lock(&peer_lock,K_FOREVER);
    struct peer_record r={.version=1,.enabled=i==0};memcpy(r.key,factory_key,16);
    zmk_2g4_addr_get_default(&r.addr);
    if(peers[1-i].record.enabled)r.addr.rf_channel=peers[1-i].record.addr.rf_channel;
    int err=save_record(i,&r);k_mutex_unlock(&peer_lock);return err;
}
int hub_peer_select(unsigned i) {
    if(i>=HUB_PEERS || !hub_peer_enabled(i))return -EINVAL;
    if(studio_radio_busy())return -EBUSY;
    uint8_t n=i;int err=settings_save_one("hub/target",&n,1);if(err)return err;
    atomic_set(&target,i);return 0;
}
int hub_peer_select_session(unsigned i) {
    if(i>=HUB_PEERS || !hub_peer_enabled(i))return -EINVAL;
    atomic_set(&target,i);return 0;
}
int zmk_2g4_crypto_reserve_session_id(void) {
    if(reserved)return 0;
    if(epoch==UINT32_MAX)return -ERANGE;
    uint32_t next=epoch+1;int err=settings_save_one("2g4/tx_epoch",&next,4);if(err)return err;
    epoch=next;reserved=true;
    for(int i=0;i<HUB_PEERS;i++)peers[i].crypto.tx_session=epoch;
    return 0;
}
void zmk_2g4_crypto_session_start(void){}
uint32_t zmk_2g4_crypto_tx_session_id(void){return epoch;}
bool zmk_2g4_crypto_enabled(void){return reserved && hub_peer_enabled(hub_peer_target());}
int hub_peer_encrypt(unsigned i,uint8_t *p,size_t n,size_t cap) {
    if(!reserved || !hub_peer_enabled(i))return -EACCES;
    k_mutex_lock(&peer_lock,K_FOREVER);int ret=peer_encrypt(&peers[i].crypto,0,p,n,cap);k_mutex_unlock(&peer_lock);return ret;
}
int hub_peer_decrypt(unsigned i,uint8_t *p,size_t n) {
    if(!reserved || !hub_peer_enabled(i))return -EACCES;
    k_mutex_lock(&peer_lock,K_FOREVER);int ret=peer_decrypt(&peers[i].crypto,1,p,n);k_mutex_unlock(&peer_lock);return ret;
}
int zmk_2g4_crypto_encrypt(uint8_t *p,size_t n,size_t cap){return hub_peer_encrypt(hub_peer_target(),p,n,cap);}
int zmk_2g4_crypto_decrypt(uint8_t *p,size_t n){return hub_peer_decrypt(hub_peer_target(),p,n);}
bool zmk_2g4_crypto_has_paired_key(void){return hub_peer_paired(hub_peer_target());}
bool zmk_2g4_crypto_uses_factory_key(void){return !zmk_2g4_crypto_has_paired_key();}
uint32_t zmk_2g4_crypto_key_id(void){return hub_peer_key_id(hub_peer_target());}
bool zmk_2g4_addr_is_paired(void){return hub_peer_paired(hub_peer_target());}
void zmk_2g4_addr_get(struct zmk_2g4_addr *a){hub_peer_addr(hub_peer_target(),a);}
// Legacy commands address the current target. PEERPAIR updates a whole slot atomically.
int zmk_2g4_crypto_set_paired_key(const uint8_t key[16]) {
    unsigned i=hub_peer_target();struct zmk_2g4_addr a;hub_peer_addr(i,&a);return hub_peer_pair(i,key,&a);
}
int zmk_2g4_addr_set_paired(const struct zmk_2g4_addr *a) {
    unsigned i=hub_peer_target();uint8_t key[16];k_mutex_lock(&peer_lock,K_FOREVER);memcpy(key,peers[i].record.key,16);k_mutex_unlock(&peer_lock);
    int err=hub_peer_pair(i,key,a);memset(key,0,16);return err;
}
int zmk_2g4_crypto_clear_paired_key(void){return hub_peer_clear(hub_peer_target());}
int zmk_2g4_addr_clear_paired(void){return 0;}
void hub_peer_status(char *out,size_t size) {
    size_t used=snprintk(out,size,"HUB_PEERS=2\r\nHUB_TARGET=%u\r\nHUB_STUDIO_OPEN=%u\r\n",hub_peer_target(),studio_radio_busy());
    for(unsigned i=0;i<HUB_PEERS && used<size;i++) {
        uint32_t rx,fail,drop;hub_keyboard_stats(i,&rx,&fail,&drop);struct zmk_2g4_addr a;hub_peer_addr(i,&a);
        int n=snprintk(out+used,size-used,"HUB_PEER=%u,%u,%u,%u,%d,%08X,%02X%02X%02X%02X%02X,%u,%u,%u,%u\r\n",
            i,hub_peer_enabled(i),hub_peer_paired(i),hub_keyboard_connected(i),hub_keyboard_battery(i),hub_peer_key_id(i),
            a.base[0],a.base[1],a.base[2],a.base[3],a.prefix,a.rf_channel,rx,fail,drop);
        used+=MIN(n,size-used-1);
        if(used<size) {
            n=snprintk(out+used,size-used,"HUB_MOTION=%u,%s\r\n",i,
                       motion_status_name(hub_keyboard_motion(i)));
            used+=MIN(n,size-used-1);
        }
    }
}
