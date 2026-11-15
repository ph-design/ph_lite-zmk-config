/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool zmk_2g4_crypto_enabled(void);
uint32_t zmk_2g4_crypto_tx_session_id(void);
int zmk_2g4_crypto_reserve_session_id(void);
void zmk_2g4_crypto_session_start(void);
int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size);
int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len);

int zmk_2g4_crypto_set_paired_key(const uint8_t key[16]);
int zmk_2g4_crypto_clear_paired_key(void);
bool zmk_2g4_crypto_uses_factory_key(void);
bool zmk_2g4_crypto_has_paired_key(void);
uint32_t zmk_2g4_crypto_key_id(void);

#define ZMK_2G4_ADDR_LEN 6

struct zmk_2g4_addr {
    uint8_t base[4];
    uint8_t prefix;
    uint8_t rf_channel;
} __packed;

void zmk_2g4_addr_get_default(struct zmk_2g4_addr *addr);
bool zmk_2g4_addr_is_paired(void);
void zmk_2g4_addr_get(struct zmk_2g4_addr *addr);
int zmk_2g4_addr_set_paired(const struct zmk_2g4_addr *addr);
int zmk_2g4_addr_clear_paired(void);
