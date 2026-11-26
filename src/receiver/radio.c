/* SPDX-License-Identifier: MIT */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zmk/2g4.h>
#include <zmk/esb.h>
#include <zmk/2g4_crypto.h>
#include <zmk/2g4_protocol.h>
#include <zmk/studio/radio_link.h>
#include "hub.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

K_MSGQ_DEFINE(radio_packets, sizeof(struct zmk_esb_payload), 32, 4);
static K_MUTEX_DEFINE(radio_lock);
static K_SEM_DEFINE(radio_ready, 0, 1);
static atomic_t management_ready;
static bool started;
static atomic_t connected;
static atomic_t battery = ATOMIC_INIT(-1);
static atomic_t last_packet, last_battery, rssi, rx_count, rx_errors, queue_drops, rx_overrun;
static atomic_t motion;

enum motion_state {
    MOTION_UNKNOWN,
    MOTION_READY = ZMK_2G4_MOTION_READY,
    MOTION_LOCKED = ZMK_2G4_MOTION_LOCKED,
};
static void session_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(session_work, session_work_handler);

static bool paired(void) { return zmk_2g4_crypto_has_paired_key() && zmk_2g4_addr_is_paired(); }
static void release_keyboard(void) {
    atomic_clear(&connected);
    atomic_set(&battery, -1);
    atomic_set(&motion, MOTION_UNKNOWN);
    hub_usb_release();
}
bool hub_keyboard_connected(void) { return atomic_get(&connected) && zmk_2g4_crypto_link_ready(); }
int hub_keyboard_battery(void) {
    uint32_t age = k_uptime_get_32() - (uint32_t)atomic_get(&last_battery);
    return hub_keyboard_connected() && age < 2000 ? atomic_get(&battery) : -1;
}

bool hub_keyboard_locked(void) {
    return hub_keyboard_connected() && atomic_get(&motion) == MOTION_LOCKED;
}
static void esb_event(const struct zmk_esb_event *event) {
    if (event->evt_id != ZMK_ESB_EVENT_RX_RECEIVED) {
        return;
    }
    struct zmk_esb_payload payload;
    bool received = false;
    while (!zmk_esb_read_rx_payload(&payload)) {
        received = true;
        atomic_inc(&rx_count);
        if (k_msgq_put(&radio_packets, &payload, K_NO_WAIT)) {
            atomic_inc(&queue_drops);
            atomic_set(&rx_overrun, 1);
        }
    }
    if (received) {
        hub_serial_radio_activity();
    }
}

uint32_t hub_radio_rx_count(void) { return atomic_get(&rx_count); }

int hub_radio_ack(const uint8_t *data, size_t len) {
    struct zmk_esb_payload payload = {.pipe = 0};
    if (len > sizeof(payload.data)) {
        return -EMSGSIZE;
    }

    k_mutex_lock(&radio_lock, K_FOREVER);
    int err = -ENOTCONN;
    if (started) {
        memcpy(payload.data, data, len);
        err = zmk_2g4_crypto_encrypt(payload.data, len, sizeof(payload.data));
        if (err > 0) {
            payload.length = err;
            err = zmk_esb_write_payload_if_idle(&payload);
        }
    }
    k_mutex_unlock(&radio_lock);
    return err;
}

static void session_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&radio_lock, K_FOREVER);
    if (started && paired()) {
        struct zmk_esb_payload payload = {.pipe = 0};
        uint8_t type;
        int len = zmk_2g4_crypto_session_tx(payload.data, sizeof(payload.data), &type);
        if (len > 0) {
            payload.length = len;
            if (!zmk_esb_write_payload_if_idle(&payload)) {
                zmk_2g4_crypto_session_sent(type);
            }
        }
    }
    k_mutex_unlock(&radio_lock);
}
int hub_radio_start(void) {
    k_mutex_lock(&radio_lock, K_FOREVER);
    release_keyboard();
    if (started) {
        zmk_esb_disable();
        started = false;
    }
    k_msgq_purge(&radio_packets);
    atomic_clear(&rx_overrun);
    if (!paired()) {
        k_mutex_unlock(&radio_lock);
        return 0;
    }

    int err = zmk_2g4_crypto_reserve_session_id();
    if (!err) {
        err = zmk_2g4_crypto_session_start();
    }
    struct zmk_esb_config config = {
        .mode = ZMK_ESB_MODE_PRX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event,
        .retransmit_delay = 500,
        .retransmit_count = 4,
    };
    if (!err) {
        err = zmk_esb_init(&config);
    }
    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);
    if (!err) {
        err = zmk_esb_set_base_address_0(addr.base);
    }
    if (!err) {
        err = zmk_esb_set_prefixes(&addr.prefix, 1);
    }
    if (!err) {
        err = zmk_esb_set_rf_channel(addr.rf_channel);
    }
    if (!err) {
        err = zmk_esb_set_tx_power(8);
    }
    if (!err) {
        err = zmk_esb_start_rx();
    }
    started = !err;
    if (err) {
        zmk_esb_disable();
    }
    k_mutex_unlock(&radio_lock);
    return err;
}
static void process_packet(struct zmk_esb_payload *payload) {
    int len = zmk_2g4_crypto_decrypt(payload->data, payload->length);
    if (len < 0) {
        atomic_inc(&rx_errors);
        return;
    }
    if (!len) {
        if (!zmk_2g4_crypto_link_ready() && atomic_get(&connected)) {
            release_keyboard();
        }
        k_work_reschedule(&session_work, K_NO_WAIT);
        return;
    }
    atomic_set(&connected, 1);
    atomic_set(&last_packet, k_uptime_get_32());
    atomic_set(&rssi, payload->rssi > 0 ? -payload->rssi : payload->rssi);
    switch (payload->data[0]) {
    case ZMK_2G4_MSG_KEYBOARD_REPORT:
    case ZMK_2G4_MSG_CONSUMER_REPORT:
    case ZMK_2G4_MSG_MOUSE_REPORT:
        hub_usb_report(payload->data[0], payload->data + 1, len - 1);
        break;
    case ZMK_2G4_MSG_KEEP_ALIVE:
        atomic_set(&battery, len >= 2 && payload->data[1] <= 100 ? payload->data[1] : -1);
        atomic_set(&last_battery, k_uptime_get_32());
        atomic_set(&motion, len >= 4 && payload->data[2] == ZMK_2G4_KEEP_ALIVE_MOTION &&
                                    payload->data[3] <= MOTION_LOCKED
                                ? payload->data[3]
                                : MOTION_UNKNOWN);
        break;
    case SR_TYPE:
        studio_radio_received(payload->data, len);
        break;
    default:
        break;
    }
}
void hub_radio_status(char *out, size_t size) {
    if (!out || !size) {
        return;
    }
    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);
    static const char *const motion_names[] = {"UNKNOWN", "READY", "LOCKED"};
    bool online = hub_keyboard_connected();
    enum motion_state state = online ? atomic_get(&motion) : MOTION_UNKNOWN;
    snprintk(out, size,
             "PAIR_STATE=%s\r\nLINK=%s\r\nRF_CH=%u\r\nKEY_ID=%08X\r\n"
             "ADDR_PAIRED=%d\r\nRF_ADDR=%02X%02X%02X%02X%02X\r\n"
             "BATTERY_PCT=%d\r\nMOTION=%s\r\n",
             paired() ? "PAIRED" : "UNPAIRED", online ? "CONNECTED" : "LOST", addr.rf_channel,
             zmk_2g4_crypto_key_id(), zmk_2g4_addr_is_paired(), addr.base[0], addr.base[1],
             addr.base[2], addr.base[3], addr.prefix, hub_keyboard_battery(), motion_names[state]);
    size_t used = strlen(out);
    if (online && used < size) {
        snprintk(out + used, size - used, "RSSI_DBM=%ld\r\n", (long)atomic_get(&rssi));
    }
}
bool hub_management_ready(void) { return atomic_get(&management_ready); }

#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
static void diagnostics_handler(struct k_work *work) {
    LOG_INF("Receiver link=%u rx=%ld auth_err=%ld queue_drop=%ld last_rssi_dbm=%ld",
            hub_keyboard_connected(), (long)atomic_get(&rx_count), (long)atomic_get(&rx_errors),
            (long)atomic_get(&queue_drops), (long)atomic_get(&rssi));
    hub_usb_log_status();
    k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(1));
}

static K_WORK_DELAYABLE_DEFINE(diagnostics_work, diagnostics_handler);
#endif

int zmk_2g4_dongle_start(int settings_status) {
    int err = settings_status;
    if (!err) {
        err = hub_radio_start();
        atomic_set(&management_ready, 1);
    }
    if (err) {
        LOG_WRN("Receiver radio unavailable: %d", err);
    }
    k_sem_give(&radio_ready);
#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
    k_work_schedule(&diagnostics_work, K_SECONDS(1));
#endif
    return err;
}

static void radio_main(void) {
    k_sem_take(&radio_ready, K_FOREVER);
    for (;;) {
        struct zmk_esb_payload payload;
        int err = k_msgq_get(&radio_packets, &payload, K_MSEC(20));
        k_mutex_lock(&radio_lock, K_FOREVER);
        if (atomic_set(&rx_overrun, 0)) {
            k_msgq_purge(&radio_packets);
            release_keyboard();
            zmk_2g4_crypto_reset_link();
        } else if (!err && started) {
            process_packet(&payload);
        }
        if (atomic_get(&connected) &&
            k_uptime_get_32() - (uint32_t)atomic_get(&last_packet) >= 500) {
            release_keyboard();
        }
        k_mutex_unlock(&radio_lock);
    }
}

K_THREAD_DEFINE(receiver_radio_thread, 2048, radio_main, NULL, NULL, NULL, 2, 0, 0);
