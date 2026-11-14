/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include <zmk/esb.h>
#include <zmk/hid.h>
#include <zmk/2g4.h>
#include <zmk/2g4_protocol.h>
#include <zmk/2g4_crypto.h>
#include "studio-radio.h"
#include <zmk/battery.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_esb_payload tx_payload;
static bool ready;
static struct k_work_delayable resend_work;
static struct k_work_delayable keepalive_work;

#define ZMK_2G4_KEEPALIVE_MS 250
static uint8_t consec_fail_count;

static void resend_work_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    zmk_2g4_send_keyboard_report();
    zmk_2g4_send_consumer_report();
}

static void keepalive_handler(struct k_work *work) {
    if (!ready) {
        return;
    }

    if (consec_fail_count >= CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES) {
        k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
        return;
    }

    tx_payload.pipe = 0;
    tx_payload.noack = false;
    tx_payload.data[0] = ZMK_2G4_MSG_KEEP_ALIVE;
    // Optional keepalive extension: battery percent, 255 = not sampled.
    // Legacy receivers ignore this body; no extra radio packets are sent.
    tx_payload.data[1] = zmk_battery_is_valid() ? zmk_battery_state_of_charge() : 255;

    int enc_len =
        zmk_2g4_crypto_encrypt(tx_payload.data, 2, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
        return;
    }
    tx_payload.length = enc_len;

    zmk_esb_write_payload(&tx_payload);

    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));
}

static void esb_event_handler(const struct zmk_esb_event *event) {
    switch (event->evt_id) {
    case ZMK_ESB_EVENT_TX_SUCCESS:
        consec_fail_count = 0;
        break;
    case ZMK_ESB_EVENT_TX_FAILED:
        zmk_esb_flush_tx();
        if (++consec_fail_count < CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES) {
            k_work_reschedule(&resend_work, K_MSEC(1));
        } else {
            LOG_WRN("2.4G: %d consecutive TX failures, dongle unreachable", consec_fail_count);
        }
        break;
    case ZMK_ESB_EVENT_RX_RECEIVED: {
        struct zmk_esb_payload rx;
        while (zmk_esb_read_rx_payload(&rx) == 0) {
            int len = zmk_2g4_crypto_decrypt(rx.data,rx.length);
            if (len > 0) studio_radio_received(rx.data,len);
        }
        break;
    }
    }
}

static int send_report(uint8_t report_type, const uint8_t *body, size_t len) {
    if (!ready) {
        return -ENODEV;
    }

    tx_payload.pipe = 0;
    tx_payload.noack = false;
    tx_payload.length = len + 1;
    tx_payload.data[0] = report_type;
    memcpy(&tx_payload.data[1], body, MIN(len, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH - 1));

    int enc_len =
        zmk_2g4_crypto_encrypt(tx_payload.data, len + 1, CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH);
    if (enc_len < 0) {
        return enc_len;
    }
    tx_payload.length = enc_len;

    int ret = zmk_esb_write_payload(&tx_payload);
    if (ret == -ENOMEM) {
        zmk_esb_flush_tx();
        ret = zmk_esb_write_payload(&tx_payload);
    }

    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));

    return ret;
}

int zmk_2g4_send_keyboard_report(void) {
    struct zmk_hid_keyboard_report *report = zmk_hid_get_keyboard_report();
    return send_report(ZMK_2G4_MSG_KEYBOARD_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

int zmk_2g4_send_consumer_report(void) {
    struct zmk_hid_consumer_report *report = zmk_hid_get_consumer_report();
    return send_report(ZMK_2G4_MSG_CONSUMER_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}

#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_2g4_send_mouse_report(void) {
    struct zmk_hid_mouse_report *report = zmk_hid_get_mouse_report();
    return send_report(ZMK_2G4_MSG_MOUSE_REPORT, (const uint8_t *)&report->body,
                       sizeof(report->body));
}
#endif

static int send_boot_announcement(void) {
    uint32_t reset_cause = 0;
    int hr = hwinfo_get_reset_cause(&reset_cause);
    if (hr == 0) {
        // Clear so a subsequent boot reads a fresh cause
        (void)hwinfo_clear_reset_cause();
    } else {
        LOG_WRN("hwinfo_get_reset_cause failed: %d", hr);
        reset_cause = 0xFFFFFFFFu;
    }

    // Body: reset_cause
    uint8_t body[8];
    sys_put_le32(reset_cause, body);
    sys_put_le32(zmk_2g4_crypto_tx_session_id(), body + 4);

    int ret = send_report(ZMK_2G4_MSG_BOOT, body, sizeof(body));
    LOG_INF("2.4G BOOT sent: reason=0x%08x session=0x%08x ret=%d", reset_cause,
            zmk_2g4_crypto_tx_session_id(), ret);
    return ret;
}

bool zmk_2g4_is_ready(void) { return ready; }

int zmk_2g4_start(void) {
    if (ready) {
        return 0;
    }

    k_work_init_delayable(&resend_work, resend_work_handler);
    k_work_init_delayable(&keepalive_work, keepalive_handler);

    struct zmk_esb_config config = {
        .mode = ZMK_ESB_MODE_PTX,
        .bitrate = ZMK_ESB_BITRATE_2MBPS,
        .crc = ZMK_ESB_CRC_16BIT,
        .tx_mode = ZMK_ESB_TXMODE_AUTO,
        .event_handler = esb_event_handler,
        .selective_auto_ack = false,
        .retransmit_delay = CONFIG_ZMK_2G4_RETRANSMIT_DELAY,
        .retransmit_count = CONFIG_ZMK_2G4_RETRANSMIT_COUNT,
    };

    int ret = zmk_esb_init(&config);
    if (ret) {
        LOG_ERR("ESB init failed: %d", ret);
        return ret;
    }

    uint8_t base_addr[4];
    uint8_t prefix[1];
    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);
    memcpy(base_addr, addr.base, sizeof(base_addr));
    prefix[0] = addr.prefix;

    zmk_esb_set_base_address_0(base_addr);
    zmk_esb_set_prefixes(prefix, 1);

    zmk_esb_set_rf_channel(addr.rf_channel);
    zmk_esb_set_tx_power(CONFIG_ZMK_2G4_TX_POWER);

    ready = true;
    LOG_INF("2.4G transport started (%s addr, ch=%u)", zmk_2g4_addr_is_paired() ? "paired" : "default",
            addr.rf_channel);

    zmk_2g4_crypto_session_start();
    studio_radio_started();
    send_boot_announcement();

    k_work_reschedule(&keepalive_work, K_MSEC(ZMK_2G4_KEEPALIVE_MS));

    return 0;
}

int zmk_2g4_stop(void) {
    if (!ready) {
        return 0;
    }

    ready = false;
    k_work_cancel_delayable(&resend_work);
    k_work_cancel_delayable(&keepalive_work);
    zmk_esb_flush_tx();
    zmk_esb_flush_rx();
    zmk_esb_disable();
    LOG_INF("2.4G transport stopped");
    return 0;
}
