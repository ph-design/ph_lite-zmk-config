/* SPDX-License-Identifier: MIT */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/services/hogp.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <string.h>
#include "ble-mouse.h"
#include "hub.h"
#include "diagnostics.h"
static struct bt_hogp hogp;
static struct bt_conn *connection;
static struct mouse_map map;
static uint8_t report_map[2048];
static size_t map_size;
static atomic_t mouse_ready,packets,parse_errors,last_error;
static atomic_t disconnect_reason,security_level,connect_error;
static bool scanning,have_bond,discovering,protocol_configured;
static atomic_t resume_error_code,subscriptions_removed;
static int64_t pair_until;
static struct k_mutex map_lock;
struct candidate { bt_addr_le_t addr; int8_t rssi; char name[24]; };
static struct candidate candidates[8];
static unsigned candidate_count;
static struct k_mutex candidate_lock;
static struct k_work_delayable scan_work;
static struct k_work map_work;
static struct k_work subscribe_work;
static struct k_work_delayable resume_work;
static struct k_work connect_work;
static bt_addr_le_t reconnect_addr;
static bool radio_paused;
static int resume_error;
static void resume_handler(struct k_work *work) {
    if(!radio_paused)return;
    int err=hub_radio_pause(false);
    resume_error=err;
    if(err){k_work_reschedule(&resume_work,K_MSEC(50));return;}
    radio_paused=false;
}
static void scan_handler(struct k_work *work);
static void subscribe_handler(struct k_work *work);
static void map_handler(struct k_work *work);
static bool name_ad(struct bt_data *d,void *ctx) {
    struct { bool hid; char name[24]; } *a=ctx;
    if(d->type==BT_DATA_UUID16_ALL || d->type==BT_DATA_UUID16_SOME) {
        for(size_t i=0;i+1<d->data_len;i+=2)if(d->data[i]==0x12 && d->data[i+1]==0x18)a->hid=true;
    } else if(d->type==BT_DATA_GAP_APPEARANCE && d->data_len==2) {
        if(d->data[0]==0xc2 && d->data[1]==3)a->hid=true;
    } else if(d->type==BT_DATA_NAME_COMPLETE || d->type==BT_DATA_NAME_SHORTENED) {
        size_t n=MIN(d->data_len,sizeof(a->name)-1);
        for(size_t i=0;i<n;i++)a->name[i]=d->data[i]>=32 && d->data[i]<127?d->data[i]:'?';
    }
    return true;
}
static int connect_addr(const bt_addr_le_t *addr) {
    hub_diagnostic_stage(30);
    if(connection)return -EISCONN;
    bt_le_scan_stop();scanning=false;
    // Temporarily release ESB slots while the first BLE connection events settle.
    // Resume even if connection setup fails, so the keyboard cannot stay paused.
    if(!hub_radio_pause(true)){radio_paused=true;k_msleep(30);}
    k_work_reschedule(&resume_work,K_SECONDS(5));
    struct bt_conn_le_create_param create=BT_CONN_LE_CREATE_PARAM_INIT(BT_CONN_LE_OPT_NONE,160,16);
    struct bt_le_conn_param params={.interval_min=12,.interval_max=24,.latency=0,.timeout=400};
    int err=bt_conn_le_create(addr,&create,&params,&connection);
    atomic_set(&last_error,err);
    if(err)k_work_reschedule(&scan_work,K_SECONDS(1));
    return err;
}
static void connect_handler(struct k_work *work){connect_addr(&reconnect_addr);}
static void scan_recv(const struct bt_le_scan_recv_info *info,struct net_buf_simple *ad) {
    if(connection || !(info->adv_props&BT_GAP_ADV_PROP_CONNECTABLE))return;
    if(have_bond && bt_le_bond_exists(BT_ID_DEFAULT,info->addr)) {
        reconnect_addr=*info->addr;k_work_submit(&connect_work);return;
    }
    if(k_uptime_get()>=pair_until)return;
    struct { bool hid; char name[24]; } fields={0};
    struct net_buf_simple_state saved;net_buf_simple_save(ad,&saved);
    bt_data_parse(ad,name_ad,&fields);net_buf_simple_restore(ad,&saved);
    if(!fields.hid)return;
    k_mutex_lock(&candidate_lock,K_FOREVER);
    unsigned index;
    for(index=0;index<candidate_count;index++)if(!bt_addr_le_cmp(info->addr,&candidates[index].addr))break;
    if(index<ARRAY_SIZE(candidates)) {
        if(index==candidate_count)candidate_count++;
        candidates[index].addr=*info->addr;candidates[index].rssi=info->rssi;
        if(fields.name[0])memcpy(candidates[index].name,fields.name,sizeof(fields.name));
    }
    k_mutex_unlock(&candidate_lock);
}
static struct bt_le_scan_cb scan_callbacks={.recv=scan_recv};
static void scan_handler(struct k_work *work) {
    if(connection)return;
    bool pairing=k_uptime_get()<pair_until;
    if(!pairing && !have_bond) {
        if(scanning)bt_le_scan_stop();
        scanning=false;return;
    }
    if(!scanning) {
        // 10% scan duty leaves most radio time available to the keyboard.
        struct bt_le_scan_param params={.type=pairing?BT_LE_SCAN_TYPE_ACTIVE:BT_LE_SCAN_TYPE_PASSIVE,
            .options=BT_LE_SCAN_OPT_NONE,.interval=160,.window=16};
        int err=bt_le_scan_start(&params,NULL);
        scanning=!err;if(err)atomic_set(&last_error,err);
    }
    k_work_reschedule(&scan_work,K_SECONDS(1));
}
static uint8_t report_received(struct bt_hogp *h,struct bt_hogp_rep_info *rep,uint8_t err,const uint8_t *data) {
    if(err || !data) {
        atomic_clear(&mouse_ready);
        atomic_set(&last_error,err?err:-ENODATA);
        atomic_inc(&subscriptions_removed);hub_mouse_release(2);
        return BT_GATT_ITER_STOP;
    }
    if(!atomic_get(&mouse_ready))return BT_GATT_ITER_CONTINUE;
    struct mouse_event e;int rc;
    k_mutex_lock(&map_lock,K_FOREVER);
    rc=mouse_map_decode(&map,bt_hogp_rep_id(rep),data,bt_hogp_rep_size(rep),&e);
    k_mutex_unlock(&map_lock);
    if(!rc){hub_mouse_event(2,&e);atomic_inc(&packets);}
    else atomic_inc(&parse_errors);
    return BT_GATT_ITER_CONTINUE;
}
static void subscribe_handler(struct k_work *work) {
    if(!connection)return;
    k_mutex_lock(&map_lock,K_FOREVER);
    int err=mouse_map_parse(&map,report_map,map_size);
    k_mutex_unlock(&map_lock);
    if(err){atomic_set(&last_error,err);return;}
    // Report protocol preserves wheel, extra buttons and non-8-bit axes.
    if(!protocol_configured || bt_hogp_pm_get(&hogp)!=BT_HIDS_PM_REPORT) {
        /* Send Report mode explicitly even if the characteristic already reads
         * Report. Some peripherals do not activate input after first pairing
         * until the host writes its desired mode. Set before synchronous cb. */
        protocol_configured=true;
        err=bt_hogp_pm_write(&hogp,BT_HIDS_PM_REPORT);
        if(!err)return; // pm_update schedules the subscription work.
        if(err!=-EOPNOTSUPP){atomic_set(&last_error,err);return;}
    }
    bool subscribed=false;
    struct bt_hogp_rep_info *r=NULL;
    while((r=bt_hogp_rep_next(&hogp,r))) {
        bool mouse=false;
        for(int i=0;i<map.field_count;i++)if(map.fields[i].id==bt_hogp_rep_id(r))mouse=true;
        if(mouse && bt_hogp_rep_type(r)==BT_HIDS_REPORT_TYPE_INPUT) {
            err=bt_hogp_rep_subscribe(&hogp,r,report_received);
            if(err && err!=-EALREADY){atomic_set(&last_error,err);return;}
            subscribed=true;
        }
    }
    atomic_set(&mouse_ready,subscribed);atomic_set(&last_error,subscribed?0:-ENOTSUP);
    if(subscribed)atomic_set(&resume_error_code,bt_hogp_exit_suspend(&hogp));
    k_work_reschedule(&resume_work,K_MSEC(100));
}
static void map_chunk(struct bt_hogp *h,uint8_t err,const uint8_t *data,size_t size,size_t offset) {
    if(!connection)return;
    if(err==BT_ATT_ERR_INVALID_OFFSET && offset==map_size && offset>0) {
        k_work_submit(&subscribe_work);return;
    }
    if(err){atomic_set(&last_error,err);return;}
    if(offset!=map_size || offset+size>sizeof(report_map)){atomic_set(&last_error,-E2BIG);return;}
    if(!data || !size){k_work_submit(&subscribe_work);return;}
    memcpy(report_map+offset,data,size);map_size+=size;
    k_work_submit(&map_work);
}
static void map_handler(struct k_work *work) {
    if(!connection)return;
    int err=bt_hogp_map_read(&hogp,map_chunk,map_size,K_NO_WAIT);
    atomic_set(&last_error,err);
}
static void ready_cb(struct bt_hogp *h){map_size=0;k_work_submit(&map_work);}
static void prep_error(struct bt_hogp *h,int err){atomic_set(&last_error,err);}
static void pm_update(struct bt_hogp *h){if(map_size)k_work_submit(&subscribe_work);}
static void discovery_done(struct bt_gatt_dm *dm,void *context) {
    int err=bt_hogp_handles_assign(dm,&hogp);bt_gatt_dm_data_release(dm);
    atomic_set(&last_error,err);
}
static void discovery_missing(struct bt_conn *conn,void *ctx){atomic_set(&last_error,-ENOTSUP);}
static void discovery_error(struct bt_conn *conn,int err,void *ctx){atomic_set(&last_error,err);}
static const struct bt_gatt_dm_cb discovery_cb={.completed=discovery_done,
    .service_not_found=discovery_missing,.error_found=discovery_error};
static void discover(struct bt_conn *conn) {
    if(discovering)return;
    discovering=true;
    int err=bt_gatt_dm_start(conn,BT_UUID_HIDS,&discovery_cb,NULL);
    atomic_set(&last_error,err);
}
static void connected_cb(struct bt_conn *conn,uint8_t err) {
    protocol_configured=false;
    hub_diagnostic_stage(40);
    atomic_set(&connect_error,err);
    if(err) {
        atomic_set(&last_error,err);
        if(connection){bt_conn_unref(connection);connection=NULL;}
        k_work_reschedule(&scan_work,K_SECONDS(1));return;
    }
    int rc=bt_conn_set_security(conn,BT_SECURITY_L2);
    if(rc)atomic_set(&last_error,rc);
    else if(bt_conn_get_security(conn)>=BT_SECURITY_L2)discover(conn);
}
static void security_changed(struct bt_conn *conn,bt_security_t level,enum bt_security_err err) {
    hub_diagnostic_stage(41);
    atomic_set(&security_level,level);
    if(err){atomic_set(&last_error,err);return;}
    if(level>=BT_SECURITY_L2){have_bond=true;discover(conn);}
}
static void disconnected_cb(struct bt_conn *conn,uint8_t reason) {
    protocol_configured=false;
    atomic_set(&disconnect_reason,reason);atomic_clear(&security_level);
    atomic_clear(&mouse_ready);hub_mouse_release(2);
    k_work_cancel(&map_work);k_work_cancel(&subscribe_work);
    bt_hogp_release(&hogp);discovering=false;map_size=0;
    if(connection){bt_conn_unref(connection);connection=NULL;}
    k_work_reschedule(&scan_work,K_SECONDS(1));
    k_work_reschedule(&resume_work,K_MSEC(50));
}
BT_CONN_CB_DEFINE(conn_callbacks)={.connected=connected_cb,.disconnected=disconnected_cb,.security_changed=security_changed};
static void bond_found(const struct bt_bond_info *info,void *ctx){have_bond=true;}
int ble_mouse_init(void) {
    k_mutex_init(&map_lock);k_mutex_init(&candidate_lock);
    k_work_init_delayable(&scan_work,scan_handler);k_work_init(&map_work,map_handler);
    k_work_init(&subscribe_work,subscribe_handler);
    k_work_init_delayable(&resume_work,resume_handler);
    k_work_init(&connect_work,connect_handler);
    const struct bt_hogp_init_params params={.ready_cb=ready_cb,.prep_error_cb=prep_error,.pm_update_cb=pm_update};
    bt_hogp_init(&hogp,&params);
    bt_le_scan_cb_register(&scan_callbacks);
    bt_foreach_bond(BT_ID_DEFAULT,bond_found,NULL);
    k_work_reschedule(&scan_work,K_MSEC(500));return 0;
}
int ble_mouse_pair(void) {
    if(connection || have_bond)return -EALREADY;
    k_mutex_lock(&candidate_lock,K_FOREVER);candidate_count=0;memset(candidates,0,sizeof(candidates));k_mutex_unlock(&candidate_lock);
    pair_until=k_uptime_get()+30000;
    k_work_reschedule(&scan_work,K_NO_WAIT);return 0;
}
int ble_mouse_connect(unsigned index) {
    if(connection || k_uptime_get()>=pair_until)return -EACCES;
    k_mutex_lock(&candidate_lock,K_FOREVER);
    if(index>=candidate_count){k_mutex_unlock(&candidate_lock);return -EINVAL;}
    bt_addr_le_t addr=candidates[index].addr;k_mutex_unlock(&candidate_lock);
    return connect_addr(&addr);
}
int ble_mouse_clear(void) {
    hub_diagnostic_stage(50);
    /* Discard stale scan results and queued auto-connect attempts. A new
     * pairing window must discover fresh advertisements after forgetting. */
    k_work_cancel(&connect_work);
    k_work_cancel_delayable(&scan_work);
    have_bond=false;pair_until=0;
    if(connection)bt_conn_disconnect(connection,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    if(scanning)bt_le_scan_stop();
    scanning=false;
    k_mutex_lock(&candidate_lock,K_FOREVER);
    candidate_count=0;memset(candidates,0,sizeof(candidates));
    k_mutex_unlock(&candidate_lock);
    return bt_unpair(BT_ID_DEFAULT,BT_ADDR_LE_ANY);
}
int ble_mouse_resume(void) {
    if(!connection || !bt_hogp_ready_check(&hogp))return -EAGAIN;
    int err=bt_hogp_exit_suspend(&hogp);
    atomic_set(&resume_error_code,err);return err;
}
void ble_mouse_status(char *out,size_t size) {
    size_t used=snprintk(out,size,"BLE_STATE=%s\r\nBLE_RX=%ld\r\nBLE_PARSE_FAIL=%ld\r\nBLE_ERROR=%ld\r\nBLE_REASON=%ld\r\nBLE_SEC=%ld\r\nBLE_CONN_ERR=%ld\r\nBLE_MAP=%u\r\nRADIO_PAUSED=%d\r\nRADIO_RESUME_ERROR=%d\r\n",
        atomic_get(&mouse_ready)?"READY":connection?"CONNECTING":scanning?"SCANNING":"IDLE",
        (long)atomic_get(&packets),(long)atomic_get(&parse_errors),(long)atomic_get(&last_error),
        (long)atomic_get(&disconnect_reason),(long)atomic_get(&security_level),
        (long)atomic_get(&connect_error),(unsigned)map_size,radio_paused,resume_error);
    if(used+1<size)used+=snprintk(out+used,size-used,"BLE_RESUME_ERROR=%ld\r\nBLE_SUBSCRIPTIONS_REMOVED=%ld\r\n",
        (long)atomic_get(&resume_error_code),(long)atomic_get(&subscriptions_removed));
    k_mutex_lock(&candidate_lock,K_FOREVER);
    for(unsigned i=0;i<candidate_count && used+1<size;i++) {
        int n=snprintk(out+used,size-used,"BLE_DEVICE=%u,%d,%s\r\n",i,candidates[i].rssi,
            candidates[i].name[0]?candidates[i].name:"BLE HID");
        used+=MIN(n,size-used-1);
    }
    k_mutex_unlock(&candidate_lock);
}
