/*
 * Copyright (c) 2026 Team PHDesign
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <string.h>
#include <strings.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>

#include <dt-bindings/zmk/reset.h>
#include <hal/nrf_power.h>
#include <zmk/2g4_crypto.h>
#include <zmk/2g4_protocol.h>
#include <zmk/studio/radio_link.h>

#include "hub.h"
#include "msg_framing.h"

LOG_MODULE_REGISTER(hub_serial);

#define COMMAND_SIZE 96
#define COMMAND_TIMEOUT_MS 100

static const struct device *const serial_dev = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));

RING_BUF_DECLARE(serial_rx, 2048);
RING_BUF_DECLARE(studio_rx, 2048);
RING_BUF_DECLARE(serial_tx, 4096);

struct serial_frame {
    bool active;
    bool escaped;
};

static struct serial_frame rx_frame, tx_frame;
static struct k_spinlock serial_lock;
static bool serial_enabled;
static char command[COMMAND_SIZE];
static size_t command_len;
static int64_t command_updated;
static bool command_ready;
static uint8_t command_end;
static bool line_start = true;
static bool skip_lf;

struct studio_packet {
    uint8_t len;
    uint8_t data[SR_HEADER + SR_DATA];
};

K_MSGQ_DEFINE(studio_packets, sizeof(struct studio_packet), 8, 4);

static struct sr_link link;
static bool studio_active;
static bool studio_synced;
static int64_t last_studio_rx;
static uint16_t connection_count;

static void frame_update(struct serial_frame *frame, uint8_t byte) {
    if (frame->escaped) {
        frame->escaped = false;
    } else if (byte == FRAMING_SOF) {
        frame->active = true;
    } else if (frame->active && byte == FRAMING_ESC) {
        frame->escaped = true;
    } else if (byte == FRAMING_EOF) {
        frame->active = false;
    }
}

static void serial_irq(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);

    k_spinlock_key_t key = k_spin_lock(&serial_lock);
    if (!serial_enabled || !uart_irq_update(dev)) {
        k_spin_unlock(&serial_lock, key);
        return;
    }

    if (uart_irq_rx_ready(dev)) {
        uint8_t data[32];
        int len;

        while (ring_buf_space_get(&serial_rx) >= sizeof(data) &&
               (len = uart_fifo_read(dev, data, sizeof(data))) > 0) {
            ring_buf_put(&serial_rx, data, len);
        }
        if (ring_buf_space_get(&serial_rx) < sizeof(data)) {
            uart_irq_rx_disable(dev);
        }
    }

    if (uart_irq_tx_ready(dev)) {
        uint8_t *data;
        uint32_t len = ring_buf_get_claim(&serial_tx, &data, 64);
        int sent = len ? uart_fifo_fill(dev, data, len) : 0;

        ring_buf_get_finish(&serial_tx, sent > 0 ? MIN((uint32_t)sent, len) : 0);
        if (sent <= 0 || ring_buf_is_empty(&serial_tx)) {
            uart_irq_tx_disable(dev);
        }
    }
    k_spin_unlock(&serial_lock, key);
}

static bool serial_is_open(void) {
    uint32_t dtr = 0;

    return !uart_line_ctrl_get(serial_dev, UART_LINE_CTRL_DTR, &dtr) && dtr;
}

static void serial_reset(bool open) {
    k_spinlock_key_t key = k_spin_lock(&serial_lock);

    serial_enabled = false;
    uart_irq_rx_disable(serial_dev);
    uart_irq_tx_disable(serial_dev);
    ring_buf_reset(&serial_rx);
    ring_buf_reset(&studio_rx);
    ring_buf_reset(&serial_tx);
    rx_frame = (struct serial_frame){0};
    tx_frame = (struct serial_frame){0};
    memset(command, 0, sizeof(command));
    command_len = 0;
    command_ready = false;
    skip_lf = false;
    line_start = true;
    serial_enabled = open;
    if (open) {
        uart_irq_rx_enable(serial_dev);
    }
    k_spin_unlock(&serial_lock, key);
}

static void forward_byte(uint8_t byte) {
    ring_buf_put(&studio_rx, &byte, 1);
    frame_update(&rx_frame, byte);
    line_start = !rx_frame.active && (byte == '\r' || byte == '\n' || byte == FRAMING_EOF);
}

static void forward_command(void) {
    for (size_t i = 0; i < command_len; i++) {
        forward_byte((uint8_t)command[i]);
    }
    memset(command, 0, sizeof(command));
    command_len = 0;
}

static void process_byte(uint8_t byte) {
    if (skip_lf) {
        skip_lf = false;
        if (byte == '\n') {
            return;
        }
    }

    if (rx_frame.active) {
        forward_byte(byte);
        return;
    }

    if (command_len) {
        if ((command_len == 1 && byte != 'T' && byte != 't') ||
            (byte != '\r' && byte != '\n' && (byte < 0x20 || byte > 0x7E))) {
            forward_command();
            forward_byte(byte);
        } else if (byte == '\r' || byte == '\n') {
            command[command_len] = '\0';
            command_end = byte;
            command_ready = true;
        } else if (command_len + 1 < sizeof(command)) {
            command[command_len++] = byte;
            command_updated = k_uptime_get();
        } else {
            forward_command();
            forward_byte(byte);
        }
    } else if (line_start && (byte == 'A' || byte == 'a')) {
        command[command_len++] = byte;
        command_updated = k_uptime_get();
    } else {
        forward_byte(byte);
    }
}

static bool serial_write(const uint8_t *data, size_t len) {
    if (ring_buf_space_get(&serial_tx) < len) {
        return false;
    }
    ring_buf_put(&serial_tx, data, len);
    uart_irq_tx_enable(serial_dev);
    return true;
}

static void reply(const char *text) {
    if (!serial_write((const uint8_t *)text, strlen(text))) {
        LOG_ERR("AT reply exceeds serial TX buffer");
    }
}

static int parse_pair(const char *args, uint8_t key[16], struct zmk_2g4_addr *addr) {
    if (strlen(args) != 46 || args[32] != ',' || args[43] != ',') {
        return -EINVAL;
    }
    uint8_t address[5], channel;
    if (hex2bin(args, 32, key, 16) != 16 ||
        hex2bin(args + 33, 10, address, sizeof(address)) != sizeof(address) ||
        hex2bin(args + 44, 2, &channel, 1) != 1 || channel > 100) {
        return -EINVAL;
    }
    memcpy(addr->base, address, sizeof(addr->base));
    addr->prefix = address[4];
    addr->rf_channel = channel;
    return 0;
}

static bool studio_busy(void) {
    return zmk_2g4_crypto_link_ready() && (studio_active || !ring_buf_is_empty(&studio_rx));
}

static bool at_match(const char *line, const char *name) {
    return strncasecmp(line, name, strlen(name) + 1) == 0;
}

static bool at_command(const char *command) {
    if (at_match(command, "AT")) {
        reply("\r\nOK\r\n");
    } else if (at_match(command, "AT+GMR")) {
        reply("\r\nph_lite_receiver 0.7.0\r\nOK\r\n");
    } else if (at_match(command, "AT+INFO") || at_match(command, "AT+2G4?")) {
        char status[512];
        hub_radio_status(status, sizeof(status));
        reply("\r\n");
        reply(status);
        snprintk(status, sizeof(status),
                 "STUDIO_2G4=1\r\nSTUDIO_ACTIVE=%d\r\nSTUDIO_PORT=SHARED_CDC\r\nBLE_MOUSE=0\r\n",
                 studio_active && studio_synced);
        reply(status);
        hub_led_status(status, sizeof(status));
        reply(status);
        reply("OK\r\n");
    } else if (at_match(command, "AT+HELP")) {
        reply("\r\nAT\r\nAT+GMR\r\nAT+INFO\r\nAT+2G4?\r\n"
              "AT+PAIR=<32HEX>,<10HEX>,<2HEX>\r\nAT+CLEAR\r\n"
              "AT+LED?\r\nAT+LED=<AUTO|ON|OFF>,<0-100>,<5-50>,<500-5000>\r\n"
              "AT+LEDSAVE\r\nAT+LEDIDENTIFY\r\nAT+RST\r\nAT+BL\r\nOK\r\n");
    } else if (!strncasecmp(command, "AT+PAIR=", 8)) {
        uint8_t key[16] = {0};
        struct zmk_2g4_addr addr;
        int err =
            !hub_management_ready() || studio_busy() ? -EBUSY : parse_pair(command + 8, key, &addr);
        if (!err) {
            err = zmk_2g4_crypto_set_paired_key(key);
        }
        if (!err) {
            err = zmk_2g4_addr_set_paired(&addr);
            if (err) {
                zmk_2g4_crypto_clear_paired_key();
            }
        }
        if (!err) {
            err = hub_radio_start();
        }
        memset(key, 0, sizeof(key));
        reply(err ? "\r\nERROR PAIR\r\n" : "\r\nOK\r\n");
    } else if (at_match(command, "AT+CLEAR")) {
        if (!hub_management_ready() || studio_busy()) {
            reply("\r\nERROR STUDIO_OPEN\r\n");
        } else {
            int key_err = zmk_2g4_crypto_clear_paired_key();
            int addr_err = zmk_2g4_addr_clear_paired();
            int radio_err = hub_radio_start();
            bool failed =
                (key_err && key_err != -ENOENT) || (addr_err && addr_err != -ENOENT) || radio_err;
            reply(failed ? "\r\nERROR CLEAR\r\n" : "\r\nOK\r\n");
        }
    } else if (at_match(command, "AT+LED?")) {
        char status[160];
        hub_led_status(status, sizeof(status));
        reply("\r\n");
        reply(status);
        reply("OK\r\n");
    } else if (!strncasecmp(command, "AT+LED=", 7)) {
        reply(hub_led_configure(command + 7) ? "\r\nERROR LED\r\n" : "\r\nOK\r\n");
    } else if (at_match(command, "AT+LEDSAVE")) {
        int err = hub_management_ready() ? hub_led_save() : -EBUSY;
        reply(err ? "\r\nERROR LED SAVE\r\n" : "\r\nOK\r\n");
    } else if (at_match(command, "AT+LEDIDENTIFY")) {
        hub_led_identify();
        reply("\r\nOK\r\n");
    } else if (at_match(command, "AT+RST")) {
        reply("\r\nOK\r\n");
        k_msleep(50);
        sys_reboot(SYS_REBOOT_COLD);
    } else if (at_match(command, "AT+BL")) {
        reply("\r\nOK\r\n");
        k_msleep(50);
        NRF_POWER->GPREGRET = RST_UF2;
        NVIC_SystemReset();
    } else {
        return false;
    }
    return true;
}

static void process_command(void) {
    /* Insert a complete AT reply between Studio frames, after queued TX drains. */
    if (!command_ready || tx_frame.active || !ring_buf_is_empty(&serial_tx)) {
        return;
    }

    if (at_command(command)) {
        skip_lf = command_end == '\r';
        line_start = true;
        memset(command, 0, sizeof(command));
        command_len = 0;
    } else {
        forward_command();
        forward_byte(command_end);
    }
    command_ready = false;
}

static void serial_poll(void) {
    process_command();
    uint8_t byte;

    /* Keep enough space to replay an entire rejected command without loss. */
    while (!command_ready && ring_buf_space_get(&studio_rx) >= sizeof(command) &&
           ring_buf_get(&serial_rx, &byte, 1)) {
        process_byte(byte);
        process_command();
    }

    if (!command_ready && command_len && k_uptime_get() - command_updated >= COMMAND_TIMEOUT_MS &&
        ring_buf_space_get(&studio_rx) >= sizeof(command)) {
        forward_command();
    }

    if (ring_buf_space_get(&serial_rx) >= 32) {
        uart_irq_rx_enable(serial_dev);
    }
    if (!ring_buf_is_empty(&serial_tx)) {
        uart_irq_tx_enable(serial_dev);
    }
}

void studio_radio_received(const uint8_t *data, size_t len) {
    if (!sr_valid(data, len)) {
        return;
    }
    struct studio_packet message = {.len = len};
    memcpy(message.data, data, len);
    k_msgq_put(&studio_packets, &message, K_NO_WAIT);
}

static void studio_stop(void) {
    studio_active = false;
    studio_synced = false;
    ring_buf_reset(&studio_rx);
    /* An abandoned response can no longer supply its frame terminator. */
    tx_frame = (struct serial_frame){0};
}

static void studio_start(void) {
    if (connection_count == UINT16_MAX) {
        return;
    }

    uint32_t session = (zmk_2g4_crypto_tx_session_id() << 16) | ++connection_count;
    sr_reset(&link, session);
    studio_synced = false;
    studio_active = true;
    last_studio_rx = k_uptime_get();
}

static void studio_ack(uint8_t flags) {
    uint8_t data[SR_HEADER + SR_DATA];
    size_t len = sr_encode(&link, flags, data);
    hub_radio_ack(data, len);
}

struct indicator_ack {
    int64_t refresh_at;
    uint32_t rx_count;
    uint8_t value;
    bool pending;
};

static bool queue_indicators(struct indicator_ack *ack) {
    if (ack->pending) {
        if (hub_radio_rx_count() == ack->rx_count) {
            return true;
        }
        ack->pending = false;
        return false;
    }

    uint8_t value = hub_usb_indicators();
    int64_t now = k_uptime_get();
    if (ack->value == value && now < ack->refresh_at) {
        return false;
    }
    const uint8_t data[] = {ZMK_2G4_ACK_LED_INDICATORS, value};
    if (hub_radio_ack(data, sizeof(data))) {
        return false;
    }
    /* PRX has one ACK payload slot. Leave the indicator there for the next packet. */
    *ack = (struct indicator_ack){
        .refresh_at = now + MSEC_PER_SEC,
        .rx_count = hub_radio_rx_count(),
        .value = value,
        .pending = true,
    };
    return true;
}

static void studio_receive(const struct studio_packet *packet) {
    if (!studio_active || sr_session(packet->data) != link.session) {
        return;
    }

    if (packet->data[1] & SR_CLOSE) {
        studio_stop();
        return;
    }

    last_studio_rx = k_uptime_get();
    studio_synced = true;
    sr_ack(&link, packet->data);

    if (!sr_data_ready(&link, packet->data, packet->len)) {
        return;
    }
    const uint8_t *data = packet->data + SR_HEADER;
    size_t len = packet->len - SR_HEADER;
    if (serial_write(data, len)) {
        for (size_t i = 0; i < len; i++) {
            frame_update(&tx_frame, data[i]);
        }
        link.rx++;
    }
}

static void serial_main(void) {
    bool ready = !IS_ENABLED(CONFIG_ZMK_USB_LOGGING) && device_is_ready(serial_dev) &&
                 !uart_irq_callback_user_data_set(serial_dev, serial_irq, NULL);
    bool was_open = false;
    struct indicator_ack ack = {0};

    for (;;) {
        bool open = ready && serial_is_open();
        if (open != was_open) {
            if (studio_active) {
                studio_ack(SR_CLOSE);
            }
            studio_stop();
            serial_reset(open);
            was_open = open;
        }

        bool online = zmk_2g4_crypto_link_ready();
        if (studio_active && (!online || k_uptime_get() - last_studio_rx > 3000)) {
            studio_stop();
        }
        if (open) {
            serial_poll();
        }
        if (!online) {
            ack = (struct indicator_ack){0};
            ring_buf_reset(&studio_rx);
            k_msleep(open ? 10 : 20);
            continue;
        }

        if (open && !studio_active && !ring_buf_is_empty(&studio_rx)) {
            studio_start();
        }
        struct studio_packet packet;
        while (k_msgq_get(&studio_packets, &packet, K_NO_WAIT) == 0) {
            studio_receive(&packet);
        }

        if (studio_active && studio_synced && !link.pending_len) {
            link.pending_len = ring_buf_get(&studio_rx, link.pending, sizeof(link.pending));
        }
        if (!queue_indicators(&ack) && studio_active) {
            studio_ack(studio_synced ? 0 : SR_OPEN);
        }
        k_msleep(studio_active ? 5 : 20);
    }
}

K_THREAD_DEFINE(studio_radio_thread, 4096, serial_main, NULL, NULL, NULL, 5, 0, 50);
