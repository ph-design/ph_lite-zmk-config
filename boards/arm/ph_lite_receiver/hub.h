/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
uint8_t hub_usb_indicators(void);
bool hub_management_ready(void);
void hub_usb_report(uint8_t id, const uint8_t *data, size_t len);
void hub_usb_release(void);
#if defined(CONFIG_ZMK_USB_LOGGING)
void hub_usb_log_status(void);
#endif
int hub_radio_start(void);
void studio_radio_received(const uint8_t *data, size_t len);
bool hub_keyboard_connected(void);
int hub_keyboard_battery(void);
bool hub_keyboard_locked(void);
void hub_radio_status(char *out, size_t size);
void hub_led_status(char *out, size_t size);
int hub_led_configure(const char *args);
int hub_led_save(void);
void hub_led_identify(void);
int hub_radio_ack(const uint8_t *data, size_t len);
uint32_t hub_radio_rx_count(void);
