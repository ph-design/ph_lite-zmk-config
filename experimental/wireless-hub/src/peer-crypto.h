/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
struct peer_crypto {
    uint8_t key[16];
    uint32_t tx_session,tx_counter,rx_session,rx_counter;
    uint64_t rx_window;
    bool rx_initialized;
};
int peer_encrypt(struct peer_crypto *s,uint8_t direction,uint8_t *data,size_t len,size_t cap);
int peer_decrypt(struct peer_crypto *s,uint8_t direction,uint8_t *data,size_t len);
