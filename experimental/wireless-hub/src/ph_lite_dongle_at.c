/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <hal/nrf_power.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include <zmk/2g4.h>
#include "led-control.h"
#include "ble-mouse.h"
#include "studio-radio.h"
#include <zmk/2g4_crypto.h>
#include "hub-peer.h"
#include "diagnostics.h"

#define AT_UART_NODE DT_NODELABEL(cdc_acm_uart0)
#define AT_LINE_MAX 96
#define ZMK_2G4_DONGLE_AT_VERSION "ph_lite_dongle 0.6.7-motion-breath"

BUILD_ASSERT(DT_NODE_HAS_STATUS(AT_UART_NODE, okay), "cdc_acm_uart0 must be enabled");

static const struct device *const at_uart = DEVICE_DT_GET(AT_UART_NODE);
static char rx_line[AT_LINE_MAX];
static size_t rx_len;
static char pending_command[AT_LINE_MAX];
static atomic_t command_pending = ATOMIC_INIT(0);
static struct k_work_q at_work_queue;
K_THREAD_STACK_DEFINE(at_work_stack, 4096);

static void at_write(const char *text) {
    unsigned int count = 0;
    while (*text != '\0') {
        uart_poll_out(at_uart, *text++);
        /* Let CDC drain long status replies without monopolizing USB/BLE work. */
        if (++count % 32 == 0) k_msleep(2);
    }
}

static void normalize_command(char *dst, size_t dst_size, const char *src) {
    size_t start = 0;
    size_t end = strlen(src);
    size_t out = 0;

    while (src[start] == ' ' || src[start] == '\t') {
        start++;
    }
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t')) {
        end--;
    }

    for (size_t pos = start; pos < end && out + 1 < dst_size; pos++) {
        char ch = src[pos];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 'A');
        }
        dst[out++] = ch;
    }

    dst[out] = '\0';
}

static bool command_is(const char *command, const char *expected) {
    return strcmp(command, expected) == 0;
}

static bool at_channel_reserved(uint8_t channel) {
    return channel == 2 || channel == 26 || channel == 80;
}

static int parse_channel_arg(const char *args, uint8_t *channel) {
    if (!*args) {
        return -EINVAL;
    }

    uint32_t value = 0;
    for (const char *p = args; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -EINVAL;
        }
        value = value * 10 + (uint32_t)(*p - '0');
        if (value > 100) {
            return -EINVAL;
        }
    }

    *channel = (uint8_t)value;
    return at_channel_reserved(*channel) ? -EINVAL : 0;
}

// AT+PAIR=<32hex key>,<10hex base+prefix>,<2hex channel>
static int parse_pair_args(const char *args, uint8_t key[16], struct zmk_2g4_addr *addr) {
    char key_hex[33] = {0};
    char addr_hex[11] = {0};
    char ch_hex[3] = {0};
    uint8_t addr_bytes[5];
    uint8_t channel;

    const char *p1 = strchr(args, ',');
    const char *p2 = p1 ? strchr(p1 + 1, ',') : NULL;
    if (!p1 || !p2 || p2 == p1 + 1 || strlen(p2 + 1) != 2) {
        return -EINVAL;
    }
    if (p1 - args != 32 || p2 - p1 - 1 != 10) {
        return -EINVAL;
    }

    memcpy(key_hex, args, 32);
    memcpy(addr_hex, p1 + 1, 10);
    memcpy(ch_hex, p2 + 1, 2);

    if (hex2bin(key_hex, 32, key, 16) != 16) {
        return -EINVAL;
    }
    if (hex2bin(addr_hex, 10, addr_bytes, sizeof(addr_bytes)) != sizeof(addr_bytes)) {
        return -EINVAL;
    }
    memcpy(addr->base, addr_bytes, sizeof(addr->base));
    addr->prefix = addr_bytes[4];

    if (hex2bin(ch_hex, 2, &channel, sizeof(channel)) != sizeof(channel) || channel > 100) {
        return -EINVAL;
    }
    addr->rf_channel = channel;

    return 0;
}

static void at_write_2g4_status(void) {
    char rsp[768];
    bool paired = zmk_2g4_crypto_has_paired_key();
    struct zmk_2g4_addr addr;
    uint32_t rx_total;
    uint32_t decrypt_fail;

    zmk_2g4_addr_get(&addr);
    zmk_2g4_dongle_rx_stats(&rx_total, &decrypt_fail);
    snprintk(rsp, sizeof(rsp),
             "PAIR_STATE=%s\r\nFACTORY_KEY=%d\r\nKEY_ID=%08X\r\n"
             "ADDR_PAIRED=%d\r\nRF_ADDR=%02X%02X%02X%02X%02X\r\nRF_CH=%u\r\n"
             "LINK=%s\r\nRX=%u\r\nRX_DECRYPT_FAIL=%u\r\n",
             paired ? "PAIRED" : "FACTORY", paired ? 0 : 1, zmk_2g4_crypto_key_id(),
             zmk_2g4_addr_is_paired() ? 1 : 0, addr.base[0], addr.base[1], addr.base[2],
             addr.base[3], addr.prefix, addr.rf_channel,
             zmk_2g4_dongle_kb_connected() ? "CONNECTED" : "LOST", rx_total, decrypt_fail);
    at_write(rsp);
    snprintk(rsp, sizeof(rsp), "BATTERY_PCT=%d\r\nLED_PIN=P0.15\r\n",
             zmk_2g4_dongle_battery_percent());
    at_write(rsp);
    dongle_led_status(rsp, sizeof(rsp));
    at_write(rsp);
    ble_mouse_status(rsp, sizeof(rsp));
    at_write(rsp);
    studio_radio_status(rsp, sizeof(rsp));
    at_write(rsp);
    hub_peer_status(rsp,sizeof(rsp));at_write(rsp);
    hub_diagnostics_status(rsp,sizeof(rsp));at_write(rsp);
}

static void at_command_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    char command[AT_LINE_MAX];
    normalize_command(command, sizeof(command), pending_command);

    if (command_is(command, "AT")) {
        at_write("\r\nOK\r\n");
    } else if (command_is(command,"AT+PEERS?")) {
        char status[768];hub_peer_status(status,sizeof(status));at_write(status);at_write("OK\r\n");
    } else if (strncmp(command,"AT+PEER=",8)==0) {
        const char *arg=command+8;int err=-EINVAL;
        if(arg[0]>='0' && arg[0]<='1' && !arg[1])err=hub_peer_select(arg[0]-'0');
        if(!err)err=hub_radio_reconfigure();
        at_write(!err?"\r\nOK\r\n":err==-EBUSY?"\r\nERROR STUDIO_OPEN\r\n":"\r\nERROR PEER\r\n");
    } else if (strncmp(command,"AT+PEERPAIR=",12)==0) {
        const char *arg=command+12;uint8_t key[16]={0};struct zmk_2g4_addr addr;int err=-EINVAL;
        if(arg[0]>='0' && arg[0]<='1' && arg[1]==',' && !parse_pair_args(arg+2,key,&addr))
            err=hub_peer_pair(arg[0]-'0',key,&addr);
        if(!err)err=hub_radio_reconfigure();
        at_write(!err?"\r\nOK\r\n":err==-EBUSY?"\r\nERROR STUDIO_OPEN\r\n":"\r\nERROR PEER_PAIR\r\n");
        memset(key,0,sizeof(key));memset(command,0,sizeof(command));memset(pending_command,0,sizeof(pending_command));
    } else if (strncmp(command,"AT+PEERCLEAR=",13)==0) {
        const char *arg=command+13;int err=-EINVAL;
        if(arg[0]>='0' && arg[0]<='1' && !arg[1])err=hub_peer_clear(arg[0]-'0');
        if(!err)err=hub_radio_reconfigure();
        at_write(!err?"\r\nOK\r\n":err==-EBUSY?"\r\nERROR STUDIO_OPEN\r\n":"\r\nERROR PEER\r\n");
    } else if (command_is(command, "AT+LED?")) {
        char status[240]; dongle_led_status(status, sizeof(status));
        at_write("\r\n"); at_write(status); at_write("OK\r\n");
    } else if (strncmp(command, "AT+LED=", 7) == 0) {
        if (dongle_led_configure(command + 7)) at_write("\r\nERROR INVALID_ARGS\r\n");
        else at_write("\r\nOK\r\n");
    } else if (command_is(command, "AT+LEDSAVE")) {
        at_write(dongle_led_save() ? "\r\nERROR STORAGE\r\n" : "\r\nOK\r\n");
    } else if (command_is(command, "AT+LEDIDENTIFY")) {
        dongle_led_identify(); at_write("\r\nOK\r\n");
    } else if (strncmp(command, "AT+BLECONNECT=", 14) == 0) {
        const char *arg=command+14;
        at_write(arg[0]>='0' && arg[0]<='7' && !arg[1] && !ble_mouse_connect(arg[0]-'0') ? "\r\nOK\r\n" : "\r\nERROR BLE\r\n");
    } else if (command_is(command, "AT+BLEPAIR")) {
        at_write(ble_mouse_pair() ? "\r\nERROR BLE\r\n" : "\r\nOK\r\n");
    } else if (command_is(command, "AT+BLERESUME")) {
        at_write(ble_mouse_resume() ? "\r\nERROR BLE\r\n" : "\r\nOK\r\n");
    } else if (command_is(command, "AT+BLECLEAR")) {
        at_write(ble_mouse_clear() ? "\r\nERROR BLE\r\n" : "\r\nOK\r\n");
    } else if (command_is(command, "AT+GMR")) {
        at_write("\r\n" ZMK_2G4_DONGLE_AT_VERSION "\r\nOK\r\n");
    } else if (command_is(command, "AT+HELP")) {
#if IS_ENABLED(CONFIG_PH_LITE_DONGLE_AT_BOOTLOADER)
        at_write("\r\nAT\r\nAT+GMR\r\nAT+HELP\r\nAT+LED?\r\nAT+LED=<AUTO|ON|OFF>,<0-100>,<5-50>,<500-5000>\r\nAT+LEDSAVE\r\nAT+LEDIDENTIFY\r\nAT+RST\r\nAT+BL\r\n"
                 "AT+PAIR=<32HEX>,<10HEX>,<2HEX>\r\nAT+CLEAR\r\nAT+CH=<0-100>\r\nAT+2G4?\r\nAT+INFO\r\nAT+BLEPAIR\r\nAT+BLECONNECT=<0-7>\r\nAT+BLECLEAR\r\nOK\r\n");
#else
        at_write("\r\nAT\r\nAT+GMR\r\nAT+HELP\r\nAT+LED?\r\nAT+LED=<AUTO|ON|OFF>,<0-100>,<5-50>,<500-5000>\r\nAT+LEDSAVE\r\nAT+LEDIDENTIFY\r\nAT+RST\r\n"
                 "AT+PAIR=<32HEX>,<10HEX>,<2HEX>\r\nAT+CLEAR\r\nAT+CH=<0-100>\r\nAT+2G4?\r\nAT+INFO\r\nAT+BLEPAIR\r\nAT+BLECONNECT=<0-7>\r\nAT+BLECLEAR\r\nOK\r\n");
#endif
    } else if (command_is(command, "AT+RST")) {
        at_write("\r\nOK\r\n");
        k_msleep(50);
        sys_reboot(SYS_REBOOT_COLD);
#if IS_ENABLED(CONFIG_PH_LITE_DONGLE_AT_BOOTLOADER)
    } else if (command_is(command, "AT+BL")) {
        // Adafruit UF2 bootloader: enter UF2/CDC mode on the next soft reset.
        NRF_POWER->GPREGRET = 0x57;
        at_write("\r\nOK\r\nBOOT\r\n");
        k_msleep(50);
        // Preserve Adafruit boot magic: Zephyr reboot handling may overwrite GPREGRET.
        NVIC_SystemReset();
#endif
    } else if (strncmp(command, "AT+CH=", 6) == 0) {
        uint8_t channel;
        if (parse_channel_arg(command + 6, &channel) < 0) {
            at_write("\r\nERROR INVALID_ARGS\r\nUSAGE: AT+CH=<0-100, except 2,26,80>\r\n");
        } else {
            struct zmk_2g4_addr addr;
            zmk_2g4_addr_get(&addr);
            addr.rf_channel = channel;

            int err = zmk_2g4_addr_set_paired(&addr);
            if (err < 0) {
                at_write("\r\nERROR STORAGE\r\n");
            } else {
                err = zmk_2g4_dongle_start();
                if (err < 0) {
                    at_write("\r\nERROR RADIO\r\n");
                } else {
                    at_write("\r\nOK\r\n");
                    at_write_2g4_status();
                }
            }
        }
    } else if (strncmp(command, "AT+PAIR=", 8) == 0) {
        uint8_t key[16];
        struct zmk_2g4_addr addr;
        const char *args = command + 8;

        if (parse_pair_args(args, key, &addr) < 0) {
            at_write("\r\nERROR INVALID_ARGS\r\n"
                     "USAGE: AT+PAIR=<32HEX KEY>,<10HEX ADDR>,<2HEX CH>\r\n");
        } else {
            int err = zmk_2g4_crypto_set_paired_key(key);
            if (!err) {
                err = zmk_2g4_addr_set_paired(&addr);
                if (err) {
                    zmk_2g4_crypto_clear_paired_key();
                }
            }

            if (err < 0) {
                at_write("\r\nERROR STORAGE\r\n");
            } else {
                err = zmk_2g4_dongle_start();
                if (err < 0) {
                    at_write("\r\nERROR RADIO\r\n");
                } else {
                    at_write("\r\nOK\r\n");
                    at_write_2g4_status();
                }
            }
        }

        // Wipe every buffer that held key material.
        memset(key, 0, sizeof(key));
        memset(command, 0, sizeof(command));
        memset(pending_command, 0, sizeof(pending_command));
    } else if (command_is(command, "AT+CLEAR")) {
        int key_err = zmk_2g4_crypto_clear_paired_key();
        int addr_err = zmk_2g4_addr_clear_paired();
        int err = (key_err < 0 && key_err != -ENOENT) ? key_err : addr_err;

        if (err < 0 && err != -ENOENT) {
            at_write("\r\nERROR STORAGE\r\n");
        } else {
            err = zmk_2g4_dongle_start();
            if (err < 0) {
                at_write("\r\nERROR RADIO\r\n");
            } else {
                at_write("\r\nOK\r\n");
                at_write_2g4_status();
            }
        }
    } else if (command_is(command, "AT+2G4?")) {
        at_write("\r\n");
        at_write_2g4_status();
        at_write("OK\r\n");
    } else if (command_is(command, "AT+INFO")) {
        at_write("\r\nPH_LITE_DONGLE\r\n" ZMK_2G4_DONGLE_AT_VERSION "\r\n");
        at_write_2g4_status();
        at_write("READY\r\nOK\r\n");
    } else {
        at_write("\r\nERROR\r\n");
    }

    atomic_clear(&command_pending);
}

K_WORK_DEFINE(at_command_work, at_command_work_handler);

static void submit_rx_line(void) {
    if (rx_len == 0) {
        return;
    }

    rx_line[rx_len] = '\0';
    if (atomic_cas(&command_pending, 0, 1)) {
        memcpy(pending_command, rx_line, rx_len + 1);
        k_work_submit_to_queue(&at_work_queue, &at_command_work);
    }
    rx_len = 0;
}

static void at_uart_callback(const struct device *dev, void *user_data) {
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        while (uart_fifo_read(dev, &byte, 1) == 1) {
            if (byte == '\r' || byte == '\n') {
                submit_rx_line();
            } else if (byte == '\b' || byte == 0x7f) {
                if (rx_len > 0) {
                    rx_len--;
                }
            } else if (byte >= 0x20 && byte <= 0x7e) {
                if (rx_len + 1 < sizeof(rx_line)) {
                    rx_line[rx_len++] = (char)byte;
                } else {
                    rx_len = 0;
                }
            }
        }
    }
}

static int zmk_2g4_at_init(void) {
    if (!device_is_ready(at_uart)) {
        return -ENODEV;
    }

    int err = uart_irq_callback_user_data_set(at_uart, at_uart_callback, NULL);
    if (err < 0) {
        return err;
    }

    k_work_queue_start(&at_work_queue, at_work_stack,
                       K_THREAD_STACK_SIZEOF(at_work_stack), 6, NULL);
    uart_irq_rx_enable(at_uart);
    return 0;
}

SYS_INIT(zmk_2g4_at_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
