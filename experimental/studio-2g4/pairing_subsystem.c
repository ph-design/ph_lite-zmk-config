/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/kernel.h>
#include <string.h>

#if IS_ENABLED(CONFIG_ZMK_2G4)

#include <zephyr/random/random.h>

#include <zmk/studio/rpc.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#include <zmk/endpoints.h>

ZMK_RPC_SUBSYSTEM(pairing)

#define PAIRING_RESPONSE(type, ...) ZMK_RPC_RESPONSE(pairing, type, __VA_ARGS__)

#define PAIRING_KEY_LEN 16
#define PAIRING_ADDR_LEN 5
// Avoid BLE advertising channels 37/38/39 (2402/2426/2480 MHz).
#define PAIRING_RESERVED_CH_37 2
#define PAIRING_RESERVED_CH_38 26
#define PAIRING_RESERVED_CH_39 80

static bool pairing_channel_reserved(uint8_t channel) {
    return channel == PAIRING_RESERVED_CH_37 || channel == PAIRING_RESERVED_CH_38 ||
           channel == PAIRING_RESERVED_CH_39;
}

static uint8_t pairing_pick_channel(void) {
    for (int i = 0; i < 32; i++) {
        uint8_t candidate = (uint8_t)(sys_rand32_get() % 101u);
        if (!pairing_channel_reserved(candidate)) {
            return candidate;
        }
    }

    return CONFIG_ZMK_2G4_RF_CHANNEL;
}

// Generate a printable, non-degenerate address for manual entry on the dongle.
static void pairing_generate_addr(struct zmk_2g4_addr *addr) {
    uint32_t base_rnd = sys_rand32_get();
    uint32_t prefix_rnd = sys_rand32_get();

    for (int i = 0; i < (int)sizeof(addr->base); i++) {
        addr->base[i] = (uint8_t)(0x21 + ((base_rnd >> (8 * i)) & 0x5D));
    }

    addr->prefix = (uint8_t)(0x21 + (prefix_rnd & 0x5D));
    if (addr->prefix == addr->base[0]) {
        // Avoid aliasing the prefix with the first base byte.
        addr->prefix = (addr->prefix == 0x7e) ? 0x21 : (uint8_t)(addr->prefix + 1);
    }

    addr->rf_channel = pairing_pick_channel();
}

static void pairing_fill_addr(zmk_pairing_GetPairStateResponse *resp) {
    struct zmk_2g4_addr addr;

    zmk_2g4_addr_get(&addr);

    resp->uses_factory_key = zmk_2g4_crypto_uses_factory_key();
    resp->key_id = zmk_2g4_crypto_key_id();
    resp->addr_paired = zmk_2g4_addr_is_paired();

    memcpy(resp->rf_addr.bytes, addr.base, sizeof(addr.base));
    resp->rf_addr.bytes[sizeof(addr.base)] = addr.prefix;
    resp->rf_addr.size = PAIRING_ADDR_LEN;
    resp->rf_channel = addr.rf_channel;
}

static zmk_studio_Response get_pair_state(const zmk_studio_Request *req) {
    LOG_DBG("");

    zmk_pairing_GetPairStateResponse resp = zmk_pairing_GetPairStateResponse_init_zero;
    pairing_fill_addr(&resp);

    return PAIRING_RESPONSE(get_pair_state, resp);
}

// Address changes take effect after a radio restart.
static void pairing_restart_radio(void) {
    if (!zmk_2g4_is_ready()) {
        return;
    }

    zmk_2g4_stop();

    int err = zmk_2g4_start();
    if (err) {
        LOG_ERR("2.4G restart after pairing change failed: %d", err);
    }
}

static zmk_studio_Response generate_and_pair(const zmk_studio_Request *req) {
    LOG_DBG("");

    uint8_t key[PAIRING_KEY_LEN];
    struct zmk_2g4_addr addr;

    sys_rand_get(key, sizeof(key));
    pairing_generate_addr(&addr);

    int err = zmk_2g4_crypto_set_paired_key(key);

    if (err == 0) {
        err = zmk_2g4_addr_set_paired(&addr);
        if (err) {
            // Roll back to the factory key if the address cannot be stored.
            LOG_ERR("Storing 2.4G address failed (%d), rolling back to factory key", err);
            int clear_err = zmk_2g4_crypto_clear_paired_key();
            if (clear_err < 0 && clear_err != -ENOENT) {
                LOG_ERR("Factory key rollback failed: %d", clear_err);
            }
            memset(key, 0, sizeof(key));
            return ZMK_RPC_SIMPLE_ERR(GENERIC);
        }
    } else {
        LOG_ERR("Storing 2.4G key failed: %d", err);
        memset(key, 0, sizeof(key));
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    pairing_restart_radio();

    zmk_pairing_GenerateAndPairResponse resp = zmk_pairing_GenerateAndPairResponse_init_zero;
    resp.key_id = zmk_2g4_crypto_key_id();

    memcpy(resp.key.bytes, key, sizeof(key));
    resp.key.size = PAIRING_KEY_LEN;

    memcpy(resp.rf_addr.bytes, addr.base, sizeof(addr.base));
    resp.rf_addr.bytes[sizeof(addr.base)] = addr.prefix;
    resp.rf_addr.size = PAIRING_ADDR_LEN;

    resp.rf_channel = addr.rf_channel;

    memset(key, 0, sizeof(key));

    return PAIRING_RESPONSE(generate_and_pair, resp);
}

static zmk_studio_Response clear_pairing(const zmk_studio_Request *req) {
    LOG_DBG("");

    int key_err = zmk_2g4_crypto_clear_paired_key();
    int addr_err = zmk_2g4_addr_clear_paired();

    // nothing stored yet is a success, not a failure
    if ((key_err < 0 && key_err != -ENOENT) || (addr_err < 0 && addr_err != -ENOENT)) {
        LOG_ERR("Clearing 2.4G pairing failed (key %d, addr %d)", key_err, addr_err);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    pairing_restart_radio();

    zmk_pairing_ClearPairingResponse resp = zmk_pairing_ClearPairingResponse_init_zero;

    return PAIRING_RESPONSE(clear_pairing, resp);
}

static zmk_studio_Response set_channel(const zmk_studio_Request *req) {
    uint32_t channel = req->subsystem.pairing.request_type.set_channel.channel;
    if (channel > 100 || pairing_channel_reserved(channel)) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    struct zmk_2g4_addr addr;
    zmk_2g4_addr_get(&addr);
    addr.rf_channel = (uint8_t)channel;

    int err = zmk_2g4_addr_set_paired(&addr);
    if (err) {
        LOG_ERR("Failed to store 2.4G channel: %d", err);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    pairing_restart_radio();

    zmk_pairing_SetChannelResponse resp = zmk_pairing_SetChannelResponse_init_zero;
    resp.channel = channel;
    return PAIRING_RESPONSE(set_channel, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(pairing, get_pair_state, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(pairing, generate_and_pair, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(pairing, clear_pairing, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(pairing, set_channel, ZMK_STUDIO_RPC_HANDLER_SECURED);

#else

// pairing_subsystem.c is only built with CONFIG_ZMK_2G4, this keeps a stray build honest.
typedef int pairing_subsystem_placeholder;

#endif /* CONFIG_ZMK_2G4 */
