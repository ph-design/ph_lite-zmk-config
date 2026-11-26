/*
 * Copyright (c) 2026 Team PHDesign
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/2g4_protocol.h>
#include <zmk/hid.h>
#include <zmk/hid_indicators.h>
#include <zmk/usb.h>
#include <zmk/usb_hid.h>

#include "hub.h"

BUILD_ASSERT(ZMK_2G4_MSG_KEYBOARD_REPORT == ZMK_HID_REPORT_ID_KEYBOARD);
BUILD_ASSERT(ZMK_2G4_MSG_CONSUMER_REPORT == ZMK_HID_REPORT_ID_CONSUMER);
BUILD_ASSERT(ZMK_2G4_MSG_MOUSE_REPORT == ZMK_HID_REPORT_ID_MOUSE);

static const uint8_t body_lengths[] = {
    [ZMK_HID_REPORT_ID_KEYBOARD - 1] = sizeof(struct zmk_hid_keyboard_report_body),
    [ZMK_HID_REPORT_ID_CONSUMER - 1] = sizeof(struct zmk_hid_consumer_report_body),
    [ZMK_HID_REPORT_ID_MOUSE - 1] = sizeof(struct zmk_hid_mouse_report_body),
};

#define MAX_REPORT_SIZE                                                                            \
    MAX(sizeof(struct zmk_hid_keyboard_report),                                                    \
        MAX(sizeof(struct zmk_hid_consumer_report), sizeof(struct zmk_hid_mouse_report)))

struct usb_report {
    uint32_t connection_id;
    uint32_t sequence;
    uint8_t len;
    uint8_t data[MAX_REPORT_SIZE];
};

K_MSGQ_DEFINE(usb_reports, sizeof(struct usb_report), 64, 4);

static atomic_t resync;
static struct k_spinlock report_lock;
static uint32_t sequence[ARRAY_SIZE(body_lengths)];

#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
static atomic_t usb_sent;
static atomic_t usb_queue_full;
static atomic_t usb_write_error;

void hub_usb_log_status(void) {
    LOG_INF("HID status=%u queued=%u sent=%ld full=%ld write_err=%ld", zmk_usb_get_status(),
            k_msgq_num_used_get(&usb_reports), (long)atomic_get(&usb_sent),
            (long)atomic_get(&usb_queue_full), (long)atomic_get(&usb_write_error));
}
#endif

static uint8_t *current_report(uint8_t id) {
    switch (id) {
    case ZMK_HID_REPORT_ID_KEYBOARD:
        return (uint8_t *)zmk_hid_get_keyboard_report();
    case ZMK_HID_REPORT_ID_CONSUMER:
        return (uint8_t *)zmk_hid_get_consumer_report();
    case ZMK_HID_REPORT_ID_MOUSE:
        return (uint8_t *)zmk_hid_get_mouse_report();
    default:
        return NULL;
    }
}

static bool usb_ready(void) {
    return zmk_usb_is_hid_ready() && zmk_usb_get_status() != USB_DC_SUSPEND;
}

uint8_t hub_usb_indicators(void) {
    const struct zmk_endpoint_instance endpoint = {.transport = ZMK_TRANSPORT_USB};
    return zmk_hid_indicators_get_profile(endpoint) & 0x1f;
}

static void queue_report(const struct usb_report *report) {
    if (k_msgq_put(&usb_reports, report, K_NO_WAIT)) {
#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
        atomic_inc(&usb_queue_full);
#endif
        atomic_set(&resync, 1);
    }
}

static void queue_snapshots(uint32_t connection_id) {
    for (uint8_t id = 1; id <= ARRAY_SIZE(body_lengths); id++) {
        struct usb_report report = {
            .connection_id = connection_id,
            .len = body_lengths[id - 1] + 1,
        };
        k_spinlock_key_t key = k_spin_lock(&report_lock);
        memcpy(report.data, current_report(id), report.len);
        report.sequence = sequence[id - 1];
        k_spin_unlock(&report_lock, key);
        queue_report(&report);
    }
}

void hub_usb_report(uint8_t id, const uint8_t *data, size_t len) {
    if (!id || id > ARRAY_SIZE(body_lengths) || len != body_lengths[id - 1]) {
        return;
    }
    struct usb_report report = {
        .connection_id = zmk_usb_get_connection_id(),
        .len = len + 1,
        .data = {id},
    };
    memcpy(report.data + 1, data, len);
    k_spinlock_key_t key = k_spin_lock(&report_lock);
    uint8_t *current = current_report(id);
    memcpy(current, report.data, report.len);
    report.sequence = ++sequence[id - 1];
    if (id == ZMK_HID_REPORT_ID_MOUSE) {
        size_t offset = offsetof(struct zmk_hid_mouse_report, body.d_x);
        memset(current + offset, 0, report.len - offset);
    }
    k_spin_unlock(&report_lock, key);

    if (usb_ready()) {
        queue_report(&report);
    } else {
        atomic_set(&resync, 1);
    }
}

void hub_usb_release(void) {
    static const uint8_t released[MAX_REPORT_SIZE - 1];
    for (uint8_t id = 1; id <= ARRAY_SIZE(body_lengths); id++) {
        hub_usb_report(id, released, body_lengths[id - 1]);
    }
}

static void usb_output(void) {
    static uint8_t tx_buffers[2][MAX_REPORT_SIZE];
    uint8_t next = 0;
    uint32_t delivered[ARRAY_SIZE(body_lengths)] = {0};
    bool have_delivered[ARRAY_SIZE(body_lengths)] = {0};
    uint32_t connection_id = 0;
    bool was_ready = false;

    for (;;) {
        if (!usb_ready()) {
            was_ready = false;
            k_msleep(20);
            continue;
        }

        uint32_t current_id = zmk_usb_get_connection_id();
        bool refresh = atomic_set(&resync, 0);
        if (!was_ready || connection_id != current_id) {
            connection_id = current_id;
            was_ready = true;
            memset(have_delivered, 0, sizeof(have_delivered));
            refresh = true;
        }
        if (refresh) {
            k_msgq_purge(&usb_reports);
            queue_snapshots(connection_id);
        }

        struct usb_report report;
        if (k_msgq_get(&usb_reports, &report, K_MSEC(20)) ||
            report.connection_id != connection_id) {
            continue;
        }
        unsigned int index = report.data[0] - 1;
        if (have_delivered[index] && (int32_t)(report.sequence - delivered[index]) <= 0) {
            continue;
        }

        /* A successful USB transfer owns this buffer until completion. */
        memcpy(tx_buffers[next], report.data, report.len);
        int err = zmk_usb_hid_send_report(tx_buffers[next], report.len, report.connection_id);
        if (err) {
#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
            atomic_inc(&usb_write_error);
#endif
            atomic_set(&resync, 1);
            k_msleep(1);
        } else {
#if IS_ENABLED(CONFIG_ZMK_USB_LOGGING)
            atomic_inc(&usb_sent);
#endif
            next ^= 1;
            delivered[index] = report.sequence;
            have_delivered[index] = true;
        }
    }
}

K_THREAD_DEFINE(usb_output_thread, 2048, usb_output, NULL, NULL, NULL, 2, 0, 0);
