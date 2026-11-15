/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>
#include <zephyr/sys/atomic.h>
#include "studio-radio.h"

#include <zmk/esb.h>
#include <zmk/hid.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>
#include <zmk/2g4_protocol.h>
#include <zmk/2g4_crypto.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators_types.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const uint8_t zero_report[32];

static bool report_is_release(const uint8_t *body, size_t len) {
    return memcmp(body, zero_report, len) == 0;
}

#define ZMK_2G4_KB_TIMEOUT_MS 500

static atomic_t battery_percent = ATOMIC_INIT(-1);
static atomic_t battery_updated_ms;
static bool kb_connected;
static bool rx_started;
static uint32_t rx_total;
static uint32_t rx_decrypt_fail;
static struct k_work_delayable kb_lost_work;

static void kb_lost_handler(struct k_work *work) {
    if (!kb_connected) {
        return;
    }
    kb_connected = false;
    atomic_set(&battery_percent, -1);
    LOG_INF("2.4G keyboard lost (rx_total=%u, decrypt_fail=%u)", rx_total, rx_decrypt_fail);

    if (zmk_usb_get_status() == USB_DC_SUSPEND) {
        return;
    }

    zmk_hid_keyboard_clear();
    zmk_hid_consumer_clear();
    if (zmk_usb_hid_send_keyboard_report()) {
        LOG_WRN("release-all keyboard report failed");
    }
    if (zmk_usb_hid_send_consumer_report()) {
        LOG_WRN("release-all consumer report failed");
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    zmk_hid_mouse_clear();
    if (zmk_usb_hid_send_mouse_report()) {
        LOG_WRN("release-all mouse report failed");
    }
#endif
}

static void process_rx_payload(const struct zmk_esb_payload *rx) {
    if (rx->length < 2) {
        return;
    }

    rx_total++;

    uint8_t buf[CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH];
    memcpy(buf, rx->data, rx->length);
    int dec_len = zmk_2g4_crypto_decrypt(buf, rx->length);
    if (dec_len < 1) {
        rx_decrypt_fail++;
        if (dec_len != -EACCES) {
            LOG_WRN("2.4G decrypt failed: ret=%d rf_len=%u", dec_len, rx->length);
        }
        return;
    }

    if (!kb_connected) {
        kb_connected = true;
        LOG_INF("2.4G keyboard connected (rf_len=%u, total=%u)", rx->length, rx_total);
    }
    k_work_reschedule(&kb_lost_work, K_MSEC(ZMK_2G4_KB_TIMEOUT_MS));

    bool usb_suspended = (zmk_usb_get_status() == USB_DC_SUSPEND);

    switch (buf[0]) {
    case 0x40:
        studio_radio_received(buf,dec_len);
        break;
    case ZMK_2G4_MSG_KEYBOARD_REPORT: {
        struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
        if (dec_len != 1 + sizeof(report->body)) {
            LOG_WRN("Invalid keyboard report length: %d", dec_len - 1);
            break;
        }
        size_t body_len = sizeof(report->body);
        memcpy(&report->body, &buf[1], body_len);
        if (usb_suspended && report_is_release(&buf[1], body_len)) {
            break;
        }
        int err = zmk_usb_hid_send_keyboard_report();
        if (err) {
            LOG_WRN("USB keyboard report send failed: %d", err);
        }
        break;
    }
    case ZMK_2G4_MSG_CONSUMER_REPORT: {
        struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
        if (dec_len != 1 + sizeof(report->body)) {
            LOG_WRN("Invalid consumer report length: %d", dec_len - 1);
            break;
        }
        size_t body_len = sizeof(report->body);
        memcpy(&report->body, &buf[1], body_len);
        if (usb_suspended && report_is_release(&buf[1], body_len)) {
            break;
        }
        int err = zmk_usb_hid_send_consumer_report();
        if (err) {
            LOG_WRN("USB consumer report send failed: %d", err);
        }
        break;
    }
#if IS_ENABLED(CONFIG_ZMK_POINTING)
    case ZMK_2G4_MSG_MOUSE_REPORT: {
        struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
        if (dec_len != 1 + sizeof(report->body)) {
            LOG_WRN("Invalid mouse report length: %d", dec_len - 1);
            break;
        }
        size_t body_len = sizeof(report->body);
        memcpy(&report->body, &buf[1], body_len);
        if (usb_suspended && report_is_release(&buf[1], body_len)) {
            break;
        }
        int err = zmk_usb_hid_send_mouse_report();
        if (err) {
            LOG_WRN("USB mouse report send failed: %d", err);
        }
        break;
    }
#endif
    case ZMK_2G4_MSG_BOOT: {
        if (dec_len >= 1 + 8) {
            uint32_t reason = sys_get_le32(&buf[1]);
            uint32_t session = sys_get_le32(&buf[5]);
            LOG_INF("Keyboard BOOT: reset_reason=0x%08x session=0x%08x", reason, session);
        } else {
            LOG_INF("Keyboard BOOT (truncated, dec_len=%d)", dec_len);
        }
        break;
    }
    case ZMK_2G4_MSG_KEEP_ALIVE:
        if (dec_len == 2 && buf[1] <= 100) {
            atomic_set(&battery_updated_ms, (atomic_val_t)k_uptime_get_32());
            atomic_set(&battery_percent, buf[1]);
        } else {
            atomic_set(&battery_percent, -1);
        }
        break;
    default:
        LOG_DBG("Unknown 2G4 message type: 0x%02x", buf[0]);
        break;
    }
}

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            process_rx_payload(&rx);
        }
        break;
    }
    case ZMK_ESB_EVENT_TX_FAILED:
        LOG_WRN("ACK payload TX failed");
        break;
    }
}

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)

static int dongle_indicator_listener(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct zmk_esb_payload ack = {
        .pipe = 0,
        .noack = false,
        .length = 2,
    };
    ack.data[0] = ZMK_2G4_ACK_LED_INDICATORS;
    ack.data[1] = (uint8_t)ev->indicators;

    int ret = zmk_esb_write_payload(&ack);
    if (ret) {
        LOG_WRN("LED indicator ACK write failed: %d", ret);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_2g4_dongle, dongle_indicator_listener);
ZMK_SUBSCRIPTION(zmk_2g4_dongle, zmk_hid_indicators_changed);

#endif /* CONFIG_ZMK_HID_INDICATORS */

int zmk_2g4_dongle_battery_percent(void) {
    uint32_t age = k_uptime_get_32() - (uint32_t)atomic_get(&battery_updated_ms);
    return kb_connected && age < 2000 ? atomic_get(&battery_percent) : -1;
}

bool zmk_2g4_dongle_kb_connected(void) { return kb_connected; }

void zmk_2g4_dongle_rx_stats(uint32_t *rx_total_out, uint32_t *decrypt_fail_out) {
    if (rx_total_out) {
        *rx_total_out = rx_total;
    }
    if (decrypt_fail_out) {
        *decrypt_fail_out = rx_decrypt_fail;
    }
}

static int zmk_2g4_dongle_init(void) {
    k_work_init_delayable(&kb_lost_work, kb_lost_handler);

    struct zmk_esb_config config = {
        .mode = ZMK_ESB_MODE_PRX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
    };

    int ret = zmk_esb_init(&config);
    if (ret) {
        LOG_ERR("ESB PRX init failed: %d", ret);
        return ret;
    }

    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);
    return 0;
}

int zmk_2g4_dongle_start(void) {
    atomic_set(&battery_percent, -1);
    struct zmk_2g4_addr addr;

    zmk_2g4_addr_get(&addr);

    if (rx_started) {
        zmk_esb_stop_rx();
        zmk_esb_flush_tx();
        zmk_esb_flush_rx();
    }

    zmk_esb_set_base_address_0(addr.base);
    zmk_esb_set_prefixes(&addr.prefix, 1);
    zmk_esb_set_rf_channel(addr.rf_channel);

    int reserve = zmk_2g4_crypto_reserve_session_id();
    if (reserve) return reserve;
    zmk_2g4_crypto_session_start();
    studio_radio_started();
    int ret = zmk_esb_start_rx();
    if (ret) {
        LOG_ERR("ESB start RX failed: %d", ret);
        return ret;
    }

    rx_started = true;
    LOG_INF("2.4G dongle listening (%s addr, ch=%u)",
            zmk_2g4_addr_is_paired() ? "paired" : "default", addr.rf_channel);
    return 0;
}

SYS_INIT(zmk_2g4_dongle_init, APPLICATION, CONFIG_ZMK_2G4_DONGLE_INIT_PRIORITY);
