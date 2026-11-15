/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <hal/nrf_ccm.h>
#include <zephyr/sys/crc.h>
#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif
#include <zmk/2g4_crypto.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define AES_KEY_SIZE 16
#define SESSION_ID_SIZE 4
#define CTR_SIZE 4
#define HEADER_SIZE (SESSION_ID_SIZE + CTR_SIZE)
#define MIC_SIZE 4
#define OVERHEAD (HEADER_SIZE + MIC_SIZE)

#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
#define CCM_DIR_TX 0
#define CCM_DIR_RX 1
#else
#define CCM_DIR_TX 1
#define CCM_DIR_RX 0
#endif
#define CCM_PDU_HDR 3
#define CCM_MAX_PAYLOAD 23
#define CCM_SCRATCH_SIZE 43
#define REPLAY_WINDOW 64

BUILD_ASSERT(sizeof(CONFIG_ZMK_2G4_AES_KEY) == 2 * AES_KEY_SIZE + 1,
             "Set CONFIG_ZMK_2G4_AES_KEY to a 32-hex key; empty would send plaintext");

static const char *key_hex = CONFIG_ZMK_2G4_AES_KEY;
static uint8_t factory_key[AES_KEY_SIZE];
static bool factory_valid;
static uint8_t aes_key[AES_KEY_SIZE];
static bool key_valid;
static bool using_paired;
static bool ccm_ok;
static uint32_t tx_session_id;
static uint32_t tx_epoch;
static uint32_t tx_counter;
static bool session_started;
static uint32_t rx_session_id;
static uint32_t rx_counter;
static uint64_t rx_window;
static bool rx_session_init;

static void reset_rx_replay_state(void) {
    rx_session_id = 0;
    rx_counter = 0;
    rx_window = 0;
    rx_session_init = false;
}

static struct k_spinlock ccm_lock;

struct ccm_cnf {
    uint8_t key[AES_KEY_SIZE];
    uint64_t counter;
    uint8_t direction;
    uint8_t iv[8];
} __packed;

static struct ccm_cnf cnf __aligned(4);
static uint8_t ccm_in[CCM_PDU_HDR + CCM_MAX_PAYLOAD + MIC_SIZE] __aligned(4);
static uint8_t ccm_out[CCM_PDU_HDR + CCM_MAX_PAYLOAD + MIC_SIZE] __aligned(4);
static uint8_t ccm_scratch[CCM_SCRATCH_SIZE] __aligned(4);

static int parse_hex_key(void) {
    if (strlen(key_hex) != 32) {
        return -EINVAL;
    }
    if (hex2bin(key_hex, 32, factory_key, AES_KEY_SIZE) != AES_KEY_SIZE) {
        return -EINVAL;
    }
    return 0;
}

static void ccm_set_cnf(const uint8_t key[AES_KEY_SIZE], uint32_t session_id, uint32_t counter,
                        uint8_t direction) {
    memcpy(cnf.key, key, AES_KEY_SIZE);
    cnf.counter = counter;
    cnf.direction = direction & 0x01;
    memset(cnf.iv, 0, sizeof(cnf.iv));
    sys_put_le32(session_id, cnf.iv);
}

static int ccm_run(bool encrypt) {
    NRF_CCM->ENABLE = CCM_ENABLE_ENABLE_Disabled;
    NRF_CCM->ENABLE = CCM_ENABLE_ENABLE_Enabled;
    NRF_CCM->MODE = ((uint32_t)(encrypt ? CCM_MODE_MODE_Encryption : CCM_MODE_MODE_Decryption)
                     << CCM_MODE_MODE_Pos) |
                    ((uint32_t)CCM_MODE_DATARATE_2Mbit << CCM_MODE_DATARATE_Pos) |
                    ((uint32_t)CCM_MODE_LENGTH_Default << CCM_MODE_LENGTH_Pos);
    NRF_CCM->CNFPTR = (uint32_t)&cnf;
    NRF_CCM->INPTR = (uint32_t)ccm_in;
    NRF_CCM->OUTPTR = (uint32_t)ccm_out;
    NRF_CCM->SCRATCHPTR = (uint32_t)ccm_scratch;
    NRF_CCM->SHORTS = 0;

    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ENDKSGEN);
    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ERROR);
    nrf_ccm_task_trigger(NRF_CCM, NRF_CCM_TASK_KSGEN);
    if (!WAIT_FOR(nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ENDKSGEN) ||
                      nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR),
                  1000, k_busy_wait(1))) {
        return -ETIMEDOUT;
    }
    if (nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR)) {
        return -EIO;
    }

    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ENDCRYPT);
    nrf_ccm_event_clear(NRF_CCM, NRF_CCM_EVENT_ERROR);
    nrf_ccm_task_trigger(NRF_CCM, NRF_CCM_TASK_CRYPT);
    if (!WAIT_FOR(nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ENDCRYPT) ||
                      nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR),
                  1000, k_busy_wait(1))) {
        return -ETIMEDOUT;
    }
    if (nrf_ccm_event_check(NRF_CCM, NRF_CCM_EVENT_ERROR)) {
        return -EIO;
    }

    return 0;
}

static int ccm_encrypt_payload(const uint8_t key[AES_KEY_SIZE], uint32_t session_id,
                               uint32_t counter, uint8_t direction, const uint8_t *pt,
                               size_t pt_len, uint8_t *ct_mic_out) {
    if (pt_len > CCM_MAX_PAYLOAD) {
        return -ENOMEM;
    }

    k_spinlock_key_t lock = k_spin_lock(&ccm_lock);
    ccm_set_cnf(key, session_id, counter, direction);
    ccm_in[0] = 0;
    ccm_in[1] = (uint8_t)pt_len;
    ccm_in[2] = 0;
    memcpy(&ccm_in[CCM_PDU_HDR], pt, pt_len);

    int ret = ccm_run(true);
    if (ret == 0) {
        memcpy(ct_mic_out, &ccm_out[CCM_PDU_HDR], pt_len + MIC_SIZE);
    }
    k_spin_unlock(&ccm_lock, lock);
    return ret;
}

static int ccm_decrypt_payload(const uint8_t key[AES_KEY_SIZE], uint32_t session_id,
                               uint32_t counter, uint8_t direction, const uint8_t *ct_mic,
                               size_t ct_len, uint8_t *pt_out, bool *mic_ok) {
    if (ct_len > CCM_MAX_PAYLOAD) {
        return -EINVAL;
    }

    k_spinlock_key_t lock = k_spin_lock(&ccm_lock);
    ccm_set_cnf(key, session_id, counter, direction);
    ccm_in[0] = 0;
    ccm_in[1] = (uint8_t)(ct_len + MIC_SIZE);
    ccm_in[2] = 0;
    memcpy(&ccm_in[CCM_PDU_HDR], ct_mic, ct_len + MIC_SIZE);

    int ret = ccm_run(false);
    if (ret == 0) {
        *mic_ok = nrf_ccm_micstatus_get(NRF_CCM);
        if (*mic_ok) {
            memcpy(pt_out, &ccm_out[CCM_PDU_HDR], ct_len);
        }
    }
    k_spin_unlock(&ccm_lock, lock);
    return ret;
}

static bool ccm_self_test(void) {
    static const uint8_t tk[AES_KEY_SIZE] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                             0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t pt[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t ct_mic[8 + MIC_SIZE];
    uint8_t out[8];
    bool ok = false;

    if (ccm_encrypt_payload(tk, 0x12345678u, 1u, 1u, pt, sizeof(pt), ct_mic)) {
        return false;
    }
    if (ccm_decrypt_payload(tk, 0x12345678u, 1u, 1u, ct_mic, sizeof(pt), out, &ok)) {
        return false;
    }
    if (!ok || memcmp(out, pt, sizeof(pt)) != 0) {
        return false;
    }

    ct_mic[0] ^= 0x01;
    if (ccm_decrypt_payload(tk, 0x12345678u, 1u, 1u, ct_mic, sizeof(pt), out, &ok)) {
        return false;
    }
    if (ok) {
        return false;
    }
    return true;
}

static int zmk_2g4_crypto_init(void) {
    ccm_ok = ccm_self_test();
    if (!ccm_ok) {
        LOG_ERR("2.4G CCM self-test failed; encryption disabled");
        return 0;
    }

    if (parse_hex_key()) {
        LOG_ERR("Invalid 2.4G AES key (need 32 hex chars)");
    } else {
        factory_valid = true;
        memcpy(aes_key, factory_key, AES_KEY_SIZE);
        key_valid = true;
        LOG_INF("2.4G AES-CCM enabled");
    }

    tx_session_id = 1;
    return 0;
}

SYS_INIT(zmk_2g4_crypto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool zmk_2g4_crypto_enabled(void) { return key_valid && ccm_ok; }

uint32_t zmk_2g4_crypto_tx_session_id(void) { return tx_session_id; }

bool zmk_2g4_crypto_uses_factory_key(void) { return key_valid && !using_paired; }

bool zmk_2g4_crypto_has_paired_key(void) { return using_paired; }

uint32_t zmk_2g4_crypto_key_id(void) {
    if (!key_valid) {
        return 0;
    }
    return crc32_ieee(aes_key, AES_KEY_SIZE);
}

static struct zmk_2g4_addr paired_addr;
static bool addr_paired;

void zmk_2g4_addr_get_default(struct zmk_2g4_addr *addr) {
    addr->base[0] = CONFIG_ZMK_2G4_ADDR_BASE_0;
    addr->base[1] = CONFIG_ZMK_2G4_ADDR_BASE_1;
    addr->base[2] = CONFIG_ZMK_2G4_ADDR_BASE_2;
    addr->base[3] = CONFIG_ZMK_2G4_ADDR_BASE_3;
    addr->prefix = CONFIG_ZMK_2G4_ADDR_PREFIX;
    addr->rf_channel = CONFIG_ZMK_2G4_RF_CHANNEL;
}

bool zmk_2g4_addr_is_paired(void) { return addr_paired; }

void zmk_2g4_addr_get(struct zmk_2g4_addr *addr) {
    if (addr_paired) {
        memcpy(addr, &paired_addr, sizeof(*addr));
    } else {
        zmk_2g4_addr_get_default(addr);
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int settings_set_2g4(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (settings_name_steq(name, "key", NULL)) {
        if (len != AES_KEY_SIZE) {
            LOG_ERR("2.4G stored key has wrong length %u", (unsigned int)len);
            return -EINVAL;
        }
        uint8_t buf[AES_KEY_SIZE];
        if (read_cb(cb_arg, buf, sizeof(buf)) != AES_KEY_SIZE) {
            return -EIO;
        }
        memcpy(aes_key, buf, AES_KEY_SIZE);
        key_valid = true;
        using_paired = true;
        return 0;
    }
    if (settings_name_steq(name, "addr", NULL)) {
        if (len != ZMK_2G4_ADDR_LEN) {
            return -EINVAL;
        }
        if (read_cb(cb_arg, &paired_addr, ZMK_2G4_ADDR_LEN) != ZMK_2G4_ADDR_LEN) {
            return -EIO;
        }
        addr_paired = true;
        return 0;
    }
    if (settings_name_steq(name, "tx_epoch", NULL)) {
        if (len != sizeof(tx_epoch)) {
            return -EINVAL;
        }
        if (read_cb(cb_arg, &tx_epoch, sizeof(tx_epoch)) != sizeof(tx_epoch)) {
            return -EIO;
        }
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_2g4, "2g4", NULL, settings_set_2g4, NULL, NULL);

int zmk_2g4_crypto_set_paired_key(const uint8_t key[AES_KEY_SIZE]) {
    int ret = settings_save_one("2g4/key", key, AES_KEY_SIZE);
    if (ret) {
        return ret;
    }
    memcpy(aes_key, key, AES_KEY_SIZE);
    key_valid = true;
    using_paired = true;
    reset_rx_replay_state();
    return 0;
}

int zmk_2g4_crypto_clear_paired_key(void) {
    int ret = settings_delete("2g4/key");
    if (ret) {
        return ret;
    }
    using_paired = false;
    if (factory_valid) {
        memcpy(aes_key, factory_key, AES_KEY_SIZE);
        key_valid = true;
    } else {
        key_valid = false;
    }
    reset_rx_replay_state();
    return 0;
}

int zmk_2g4_addr_set_paired(const struct zmk_2g4_addr *addr) {
    int ret = settings_save_one("2g4/addr", addr, ZMK_2G4_ADDR_LEN);
    if (ret) {
        return ret;
    }
    memcpy(&paired_addr, addr, sizeof(paired_addr));
    addr_paired = true;
    reset_rx_replay_state();
    return 0;
}

int zmk_2g4_addr_clear_paired(void) {
    int ret = settings_delete("2g4/addr");
    if (ret) {
        return ret;
    }
    addr_paired = false;
    reset_rx_replay_state();
    return 0;
}
#else
int zmk_2g4_crypto_set_paired_key(const uint8_t key[AES_KEY_SIZE]) {
    ARG_UNUSED(key);
    return -ENOTSUP;
}

int zmk_2g4_crypto_clear_paired_key(void) { return -ENOTSUP; }

int zmk_2g4_addr_set_paired(const struct zmk_2g4_addr *addr) {
    ARG_UNUSED(addr);
    return -ENOTSUP;
}

int zmk_2g4_addr_clear_paired(void) { return -ENOTSUP; }
#endif

static uint32_t reserved_session_id;

int zmk_2g4_crypto_reserve_session_id(void) {
    if (session_started) {
        return 0;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    uint32_t next = tx_epoch + 1;
    if (next == 0) {
        next = 1;
    }

    // Flash writes need the BLE ticker while dual-radio mode is using it.
    int ret = settings_save_one("2g4/tx_epoch", &next, sizeof(next));
    if (ret) {
        LOG_ERR("2.4G tx_epoch persist failed: %d", ret);
        return ret;
    }

    tx_epoch = next;
    reserved_session_id = next;
#endif

    return 0;
}

void zmk_2g4_crypto_session_start(void) {
    if (session_started) {
        return;
    }
    session_started = true;

#if IS_ENABLED(CONFIG_SETTINGS)
    // Never write flash here: this runs with the BLE controller already disabled.
    tx_session_id = reserved_session_id ? reserved_session_id : (tx_epoch + 1);
    if (tx_session_id == 0) {
        tx_session_id = 1;
    }
    reserved_session_id = 0;
#else
    do {
        tx_session_id = sys_rand32_get();
    } while (tx_session_id == 0);
#endif
}

int zmk_2g4_crypto_encrypt(uint8_t *data, size_t len, size_t buf_size) {
    __ASSERT_NO_MSG(data != NULL);

    if (!key_valid) {
        return -EACCES;
    }
    if (!ccm_ok) {
        return -EIO;
    }
    if (len + OVERHEAD > buf_size) {
        return -ENOMEM;
    }

    k_spinlock_key_t lock = k_spin_lock(&ccm_lock);
    uint32_t ctr = tx_counter;
    if (ctr != 0xFFFFFFFFu) {
        tx_counter = ctr + 1;
    }
    k_spin_unlock(&ccm_lock, lock);
    if (ctr == 0xFFFFFFFFu) {
        LOG_ERR("2.4G TX counter exhausted, reboot required");
        return -ERANGE;
    }

    uint8_t ct_mic[CCM_MAX_PAYLOAD + MIC_SIZE];

    int ret = ccm_encrypt_payload(aes_key, tx_session_id, ctr, CCM_DIR_TX, data, len, ct_mic);
    if (ret) {
        return ret;
    }

    sys_put_le32(tx_session_id, data);
    sys_put_le32(ctr, data + SESSION_ID_SIZE);
    memcpy(data + HEADER_SIZE, ct_mic, len + MIC_SIZE);

    return (int)(len + OVERHEAD);
}

static bool replay_accept(uint32_t ctr) {
    if (ctr > rx_counter) {
        uint32_t shift = ctr - rx_counter;
        rx_window = (shift >= REPLAY_WINDOW) ? 0 : (rx_window << shift);
        rx_window |= 1ULL;
        rx_counter = ctr;
        return true;
    }
    uint32_t diff = rx_counter - ctr;
    if (diff >= REPLAY_WINDOW) {
        return false;
    }
    if (rx_window & (1ULL << diff)) {
        return false;
    }
    rx_window |= (1ULL << diff);
    return true;
}

int zmk_2g4_crypto_decrypt(uint8_t *data, size_t len) {
    __ASSERT_NO_MSG(data != NULL);

    if (!key_valid) {
        return -EACCES;
    }
    if (!ccm_ok) {
        return -EIO;
    }
    if (len <= OVERHEAD) {
        return -EINVAL;
    }

    uint32_t session_id = sys_get_le32(data);
    uint32_t ctr = sys_get_le32(data + SESSION_ID_SIZE);
    size_t ct_len = len - OVERHEAD;

    uint8_t pt[CCM_MAX_PAYLOAD];
    bool mic_ok = false;
    int ret = ccm_decrypt_payload(aes_key, session_id, ctr, CCM_DIR_RX, data + HEADER_SIZE,
                                  ct_len, pt, &mic_ok);
    if (ret) {
        return ret;
    }
    if (!mic_ok) {
        return -EBADMSG;
    }

    bool new_session = !rx_session_init || (session_id != rx_session_id);
    if (new_session) {
        if (rx_session_init && (int32_t)(session_id - rx_session_id) <= 0) {
            return -EACCES;
        }
    } else if (!replay_accept(ctr)) {
        return -EACCES;
    }

    memcpy(data, pt, ct_len);

    if (new_session) {
        rx_session_id = session_id;
        rx_session_init = true;
        rx_counter = ctr;
        rx_window = 1ULL;
    }

    return (int)ct_len;
}
