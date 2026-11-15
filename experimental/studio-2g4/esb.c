/*
 * Copyright (c) 2026 Team PHDesign
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_ppi.h>
#include <hal/nrf_clock.h>
#include <nrfx.h>
#include <nrfx_timer.h>
#include <nrfx_ppi.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zmk_esb, CONFIG_ZMK_ESB_LOG_LEVEL);

#include <zmk/esb.h>

#define RADIO_BASE_FREQ 2400UL
#define PID_MAX 3

#define RETRANSMIT_DELAY_MIN 500

#define ESB_USE_FAST_RAMP_UP 1

#define ACK_TIMEOUT_2MBPS 350
#define ACK_TIMEOUT_1MBPS 600

#define ESB_TIMER_IRQ_PRIO 2
#define ESB_RADIO_IRQ_PRIO 1
#define ESB_TX_TIME_US 200

static const int8_t valid_tx_powers[] = {8, 4, 3, 0, -4, -8, -12, -16, -20, -40};

static NRF_TIMER_Type *esb_timer_inst;
static IRQn_Type esb_timer_irqn;
static nrf_ppi_channel_t ppi_ch_timer_disable;

struct esb_pdu {
    uint8_t length : 6;
    uint8_t _rfu0 : 2;
    uint8_t ack : 1;
    uint8_t pid : 2;
    uint8_t _rfu1 : 5;
    uint8_t data[];
} __packed;

enum esb_state {
    STATE_UNINIT,
    STATE_IDLE,
    STATE_PTX_TX,
    STATE_PTX_RX_ACK,
    STATE_PRX,
    STATE_PRX_SEND_ACK,
};

struct pipe_info {
    uint16_t crc;
    uint8_t pid;
};

static volatile enum esb_state esb_state = STATE_UNINIT;
static struct zmk_esb_config esb_cfg;
static zmk_esb_event_handler_t evt_handler;
static uint32_t original_vtor;

static struct zmk_esb_payload tx_fifo_buf[CONFIG_ZMK_ESB_TX_FIFO_SIZE];
static uint32_t tx_front, tx_back;
static atomic_t tx_count;

static struct zmk_esb_payload rx_fifo_buf[CONFIG_ZMK_ESB_RX_FIFO_SIZE];
static uint32_t rx_front, rx_back;
static atomic_t rx_count;

static struct zmk_esb_payload ack_pl[CONFIG_ZMK_ESB_PIPE_COUNT];
static bool ack_pl_pending[CONFIG_ZMK_ESB_PIPE_COUNT];

static uint8_t tx_buf[sizeof(struct esb_pdu) + CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH] __aligned(4);
static uint8_t rx_buf[sizeof(struct esb_pdu) + CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH] __aligned(4);

static struct zmk_esb_payload *current_payload;
static volatile uint32_t retransmits_remaining;
static volatile uint32_t last_tx_attempts;
static uint8_t pids[CONFIG_ZMK_ESB_PIPE_COUNT];
static struct pipe_info rx_pipe_info[CONFIG_ZMK_ESB_PIPE_COUNT];
static uint32_t ack_timeout_us;
static int8_t tx_power_dbm;
static uint32_t watchdog_timeout_ms;

K_MSGQ_DEFINE(evt_msgq, sizeof(struct zmk_esb_event), 4, 4);
static struct k_work evt_work;
static struct k_work_delayable watchdog_work;

static void (*on_radio_disabled)(void);

static struct {
    uint8_t base_addr_p0[4];
    uint8_t base_addr_p1[4];
    uint8_t pipe_prefixes[CONFIG_ZMK_ESB_PIPE_COUNT];
    uint8_t addr_length;
    uint8_t num_pipes;
    uint8_t rx_pipes_enabled;
    uint32_t rf_channel;
} esb_addr = {
    .base_addr_p0 = {0xE7, 0xE7, 0xE7, 0xE7},
    .base_addr_p1 = {0xC2, 0xC2, 0xC2, 0xC2},
    .pipe_prefixes = {0xE7, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8},
    .addr_length = 5,
    .num_pipes = CONFIG_ZMK_ESB_PIPE_COUNT,
    .rx_pipes_enabled = 0xFF,
    .rf_channel = CONFIG_ZMK_ESB_DEFAULT_RF_CHANNEL,
};

static void on_disabled_ptx_tx(void);
static void on_disabled_ptx_rx_ack(void);
static void on_disabled_prx(void);
static void on_disabled_prx_ack_sent(void);
static void start_tx_transaction(void);
static void retransmit_current(void);
static void watchdog_start(void);
static void watchdog_cancel(void);

static uint32_t bytewise_bit_swap(const uint8_t *input) {
    uint32_t inp;

    memcpy(&inp, input, sizeof(inp));
#if __CORTEX_M >= 0x03U
    return sys_cpu_to_be32((uint32_t)__RBIT(inp));
#else
    inp = (inp & 0xF0F0F0F0) >> 4 | (inp & 0x0F0F0F0F) << 4;
    inp = (inp & 0xCCCCCCCC) >> 2 | (inp & 0x33333333) << 2;
    inp = (inp & 0xAAAAAAAA) >> 1 | (inp & 0x55555555) << 1;
    return inp;
#endif
}

static uint32_t addr_conv(const uint8_t *addr) { return __REV(bytewise_bit_swap(addr)); }

static void tx_fifo_reset(void) {
    tx_front = tx_back = 0;
    atomic_clear(&tx_count);
}

static void rx_fifo_reset(void) {
    rx_front = rx_back = 0;
    atomic_clear(&rx_count);
}

static void tx_fifo_remove_first(void) {
    if (atomic_get(&tx_count) == 0) {
        return;
    }
    if (++tx_front >= CONFIG_ZMK_ESB_TX_FIFO_SIZE) {
        tx_front = 0;
    }
    atomic_dec(&tx_count);
}

static bool rx_fifo_push(const struct esb_pdu *pdu, uint8_t pipe, int8_t rssi, uint8_t pid) {
    if (atomic_get(&rx_count) >= CONFIG_ZMK_ESB_RX_FIFO_SIZE) {
        return false;
    }
    struct zmk_esb_payload *p = &rx_fifo_buf[rx_back];

    p->length = pdu->length;
    p->pipe = pipe;
    p->rssi = rssi;
    p->pid = pid;
    p->noack = !pdu->ack;
    if (pdu->length > 0 && pdu->length <= CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH) {
        memcpy(p->data, pdu->data, pdu->length);
    }
    if (++rx_back >= CONFIG_ZMK_ESB_RX_FIFO_SIZE) {
        rx_back = 0;
    }
    atomic_inc(&rx_count);
    return true;
}

static void evt_work_handler(struct k_work *work) {
    struct zmk_esb_event evt;

    while (k_msgq_get(&evt_msgq, &evt, K_NO_WAIT) == 0) {
        if (evt_handler) {
            evt_handler(&evt);
        }
    }
}

static void signal_event(enum zmk_esb_event_id id) {
    struct zmk_esb_event evt = {
        .evt_id = id,
        .tx_attempts = last_tx_attempts,
    };
    k_msgq_put(&evt_msgq, &evt, K_NO_WAIT);
    k_work_submit(&evt_work);
}

static void configure_radio(void) {
    nrf_radio_mode_set(NRF_RADIO, esb_cfg.bitrate == ZMK_ESB_BITRATE_2MBPS
                                      ? NRF_RADIO_MODE_NRF_2MBIT
                                      : NRF_RADIO_MODE_NRF_1MBIT);

    nrf_radio_packet_conf_t pkt = {
        .s0len = 0,
        .lflen = (CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH <= 32) ? 6 : 8,
        .s1len = 3,
        .s1incl = true,
        .whiteen = false,
        .big_endian = true,
        .balen = esb_addr.addr_length - 1,
        .statlen = 0,
        .maxlen = CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH,
#if defined(RADIO_PCNF0_PLEN_Msk)
        .plen = (esb_cfg.bitrate == ZMK_ESB_BITRATE_2MBPS) ? NRF_RADIO_PREAMBLE_LENGTH_16BIT
                                                           : NRF_RADIO_PREAMBLE_LENGTH_8BIT,
#endif
    };
    nrf_radio_packet_configure(NRF_RADIO, &pkt);

    switch (esb_cfg.crc) {
    case ZMK_ESB_CRC_16BIT:
        nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_INCLUDE, 0x11021UL);
        nrf_radio_crcinit_set(NRF_RADIO, 0xFFFFUL);
        break;
    case ZMK_ESB_CRC_8BIT:
        nrf_radio_crc_configure(NRF_RADIO, 1, NRF_RADIO_CRC_ADDR_INCLUDE, 0x107UL);
        nrf_radio_crcinit_set(NRF_RADIO, 0xFFUL);
        break;
    default:
        nrf_radio_crc_configure(NRF_RADIO, 0, NRF_RADIO_CRC_ADDR_INCLUDE, 0);
        nrf_radio_crcinit_set(NRF_RADIO, 0);
        break;
    }

    nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)tx_power_dbm);

#if defined(RADIO_MODECNF0_RU_Msk) && ESB_USE_FAST_RAMP_UP
    nrf_radio_modecnf0_set(NRF_RADIO, true, 0);
#endif

    nrf_radio_base0_set(NRF_RADIO, addr_conv(esb_addr.base_addr_p0));
    nrf_radio_base1_set(NRF_RADIO, addr_conv(esb_addr.base_addr_p1));
    nrf_radio_prefix0_set(NRF_RADIO, bytewise_bit_swap(&esb_addr.pipe_prefixes[0]));
    if (esb_addr.num_pipes > 4) {
        nrf_radio_prefix1_set(NRF_RADIO, bytewise_bit_swap(&esb_addr.pipe_prefixes[4]));
    }

    nrf_radio_frequency_set(NRF_RADIO, RADIO_BASE_FREQ + esb_addr.rf_channel);
}

static void timer_init(void) {
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_CLEAR);

    nrf_timer_mode_set(esb_timer_inst, NRF_TIMER_MODE_TIMER);
    nrf_timer_bit_width_set(esb_timer_inst, NRF_TIMER_BIT_WIDTH_16);
    nrf_timer_prescaler_set(esb_timer_inst, NRF_TIMER_FREQ_1MHz);

    nrf_timer_int_disable(esb_timer_inst, 0xFFFFFFFF);
}

static void timer_deinit(void) {
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_STOP);
    nrf_timer_int_disable(esb_timer_inst, 0xFFFFFFFF);
}

static void ppi_ack_timeout_enable(void) {
    nrf_ppi_channel_endpoint_setup(
        NRF_PPI, ppi_ch_timer_disable,
        (uint32_t)nrf_timer_event_address_get(esb_timer_inst, NRF_TIMER_EVENT_COMPARE0),
        (uint32_t)nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_DISABLE));
    nrf_ppi_channel_enable(NRF_PPI, ppi_ch_timer_disable);
}

static void ppi_ack_timeout_disable(void) {
    nrf_ppi_channel_disable(NRF_PPI, ppi_ch_timer_disable);
}

static int hfclk_start(void) {
    if (nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY)) {
        return 0;
    }
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);

    if (!WAIT_FOR(nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY), 10000,
                  k_busy_wait(1))) {
        LOG_ERR("HFCLK start timed out");
        return -ETIMEDOUT;
    }

    return 0;
}

static void start_tx_transaction(void) {
    if (atomic_get(&tx_count) == 0) {
        esb_state = STATE_IDLE;
        return;
    }

    if (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
        LOG_WRN("RADIO not disabled (state=%u) before TX, forcing", NRF_RADIO->STATE);
        nrf_radio_shorts_set(NRF_RADIO, 0);
        nrf_radio_int_disable(NRF_RADIO, 0xFFFFFFFF);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        uint32_t t = 0;
        while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
            if (++t > 10000) {
                LOG_ERR("RADIO disable timeout, deferring TX");
                esb_state = STATE_IDLE;
                return;
            }
            k_busy_wait(1);
        }
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    }

    current_payload = &tx_fifo_buf[tx_front];
    struct esb_pdu *pdu = (struct esb_pdu *)tx_buf;

    memset(pdu, 0, sizeof(*pdu));
    pdu->length = current_payload->length;
    pdu->pid = current_payload->pid;
    pdu->ack = !current_payload->noack;

    memcpy(pdu->data, current_payload->data, current_payload->length);

    uint32_t shorts = NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_DISABLE_MASK;
    if (!current_payload->noack) {
        shorts |= NRF_RADIO_SHORT_DISABLED_RXEN_MASK;
    }
    nrf_radio_shorts_set(NRF_RADIO, shorts);

    nrf_radio_txaddress_set(NRF_RADIO, current_payload->pipe);
    nrf_radio_rxaddresses_set(NRF_RADIO, BIT(current_payload->pipe));
    nrf_radio_packetptr_set(NRF_RADIO, pdu);

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);
    on_radio_disabled = on_disabled_ptx_tx;
    esb_state = STATE_PTX_TX;

    retransmits_remaining = esb_cfg.retransmit_count;

    watchdog_start();

    NVIC_ClearPendingIRQ(RADIO_IRQn);
    irq_enable(RADIO_IRQn);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
}

static void retransmit_current(void) {
    uint32_t shorts = NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_DISABLE_MASK;
    if (!current_payload->noack) {
        shorts |= NRF_RADIO_SHORT_DISABLED_RXEN_MASK;
    }
    nrf_radio_shorts_set(NRF_RADIO, shorts);

    nrf_radio_packetptr_set(NRF_RADIO, tx_buf);

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);
    on_radio_disabled = on_disabled_ptx_tx;
    esb_state = STATE_PTX_TX;

    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
}

static void on_disabled_ptx_tx(void) {
    bool wants_ack = !current_payload->noack;

    if (!wants_ack) {
        last_tx_attempts = 1;
        signal_event(ZMK_ESB_EVENT_TX_SUCCESS);
        tx_fifo_remove_first();

        if (atomic_get(&tx_count) > 0 && esb_cfg.tx_mode == ZMK_ESB_TXMODE_AUTO) {
            start_tx_transaction();
        } else {
            watchdog_cancel();
            esb_state = STATE_IDLE;
        }
        return;
    }

    nrf_radio_packetptr_set(NRF_RADIO, rx_buf);

    nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
                                        NRF_RADIO_SHORT_END_DISABLE_MASK |
                                        NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_CLEAR);
    nrf_timer_cc_set(esb_timer_inst, NRF_TIMER_CC_CHANNEL0, ack_timeout_us);
    nrf_timer_event_clear(esb_timer_inst, NRF_TIMER_EVENT_COMPARE0);
    ppi_ack_timeout_enable();
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_START);

    on_radio_disabled = on_disabled_ptx_rx_ack;
    esb_state = STATE_PTX_RX_ACK;
}

static void on_disabled_ptx_rx_ack(void) {
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_CLEAR);
    ppi_ack_timeout_disable();

    bool ack_ok = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END) &&
                  nrf_radio_crc_status_check(NRF_RADIO);

    if (ack_ok) {
        struct esb_pdu *rx_pdu = (struct esb_pdu *)rx_buf;

        if (rx_pdu->length > 0 && rx_pdu->length <= CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH) {
            uint8_t pipe = nrf_radio_txaddress_get(NRF_RADIO);
            int8_t rssi = (int8_t)nrf_radio_rssi_sample_get(NRF_RADIO);

            rx_fifo_push(rx_pdu, pipe, rssi, rx_pdu->pid);
            signal_event(ZMK_ESB_EVENT_RX_RECEIVED);
        }

        last_tx_attempts = esb_cfg.retransmit_count - retransmits_remaining + 1;
        signal_event(ZMK_ESB_EVENT_TX_SUCCESS);
        tx_fifo_remove_first();

        if (atomic_get(&tx_count) > 0 && esb_cfg.tx_mode == ZMK_ESB_TXMODE_AUTO) {
            watchdog_cancel();
            start_tx_transaction();
        } else {
            watchdog_cancel();
            esb_state = STATE_IDLE;
        }
    } else {
        if (retransmits_remaining > 0) {
            retransmits_remaining--;
            LOG_DBG("Retransmit (%u remaining)", retransmits_remaining);

            if (esb_cfg.retransmit_delay > 0) {
                nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_CLEAR);
                nrf_timer_cc_set(esb_timer_inst, NRF_TIMER_CC_CHANNEL1, esb_cfg.retransmit_delay);
                nrf_timer_event_clear(esb_timer_inst, NRF_TIMER_EVENT_COMPARE1);
                nrf_timer_int_enable(esb_timer_inst, NRF_TIMER_INT_COMPARE1_MASK);
                nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_START);
                on_radio_disabled = NULL;
            } else {
                retransmit_current();
            }
        } else {
            last_tx_attempts = esb_cfg.retransmit_count + 1;
            signal_event(ZMK_ESB_EVENT_TX_FAILED);
            tx_fifo_remove_first();

            if (atomic_get(&tx_count) > 0 && esb_cfg.tx_mode == ZMK_ESB_TXMODE_AUTO) {
                watchdog_cancel();
                start_tx_transaction();
            } else {
                watchdog_cancel();
                esb_state = STATE_IDLE;
            }
        }
    }
}

static void start_rx_listening(void) {
    nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
                                        NRF_RADIO_SHORT_END_DISABLE_MASK |
                                        NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

    nrf_radio_rxaddresses_set(NRF_RADIO, esb_addr.rx_pipes_enabled);
    nrf_radio_frequency_set(NRF_RADIO, RADIO_BASE_FREQ + esb_addr.rf_channel);
    nrf_radio_packetptr_set(NRF_RADIO, rx_buf);

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);

    on_radio_disabled = on_disabled_prx;
    esb_state = STATE_PRX;

    NVIC_ClearPendingIRQ(RADIO_IRQn);
    irq_enable(RADIO_IRQn);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void on_disabled_prx(void) {
    struct esb_pdu *rx_pdu = (struct esb_pdu *)rx_buf;
    struct esb_pdu *tx_pdu = (struct esb_pdu *)tx_buf;

    if (!nrf_radio_crc_status_check(NRF_RADIO)) {
        nrf_radio_packetptr_set(NRF_RADIO, rx_buf);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
        return;
    }

    uint8_t pipe = nrf_radio_rxmatch_get(NRF_RADIO);
    uint16_t crc = nrf_radio_rxcrc_get(NRF_RADIO);
    uint8_t pid_rx = rx_pdu->pid;
    bool is_duplicate = (crc == rx_pipe_info[pipe].crc && pid_rx == rx_pipe_info[pipe].pid);

    rx_pipe_info[pipe].crc = crc;
    rx_pipe_info[pipe].pid = pid_rx;

    bool send_rx_event = false;

    if (!is_duplicate) {
        int8_t rssi = (int8_t)nrf_radio_rssi_sample_get(NRF_RADIO);

        if (rx_fifo_push(rx_pdu, pipe, rssi, pid_rx)) {
            send_rx_event = true;
        }
    }

    bool wants_ack = (esb_cfg.selective_auto_ack == false) || (rx_pdu->ack != 0);

    if (wants_ack) {
        memset(tx_pdu, 0, sizeof(struct esb_pdu));
        tx_pdu->pid = pid_rx;
        tx_pdu->ack = rx_pdu->ack;

        if (pipe < CONFIG_ZMK_ESB_PIPE_COUNT && ack_pl_pending[pipe]) {
            tx_pdu->length = ack_pl[pipe].length;
            memcpy(tx_pdu->data, ack_pl[pipe].data, ack_pl[pipe].length);
            ack_pl_pending[pipe] = false;
        } else {
            tx_pdu->length = 0;
        }

        nrf_radio_shorts_set(NRF_RADIO,
                             NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_DISABLE_MASK);
        nrf_radio_txaddress_set(NRF_RADIO, pipe);
        nrf_radio_packetptr_set(NRF_RADIO, tx_pdu);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

        on_radio_disabled = on_disabled_prx_ack_sent;
        esb_state = STATE_PRX_SEND_ACK;

        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
    } else {
        nrf_radio_packetptr_set(NRF_RADIO, rx_buf);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
        nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
    }

    if (send_rx_event) {
        signal_event(ZMK_ESB_EVENT_RX_RECEIVED);
    }
}

static void on_disabled_prx_ack_sent(void) {
    nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK |
                                        NRF_RADIO_SHORT_END_DISABLE_MASK |
                                        NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);
    nrf_radio_packetptr_set(NRF_RADIO, rx_buf);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

    on_radio_disabled = on_disabled_prx;
    esb_state = STATE_PRX;

    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void radio_isr(const void *arg) {
    ARG_UNUSED(arg);

    if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
        if (on_radio_disabled) {
            on_radio_disabled();
        }
    }
}

static void timer_isr(const void *arg) {
    ARG_UNUSED(arg);

    if (nrf_timer_event_check(esb_timer_inst, NRF_TIMER_EVENT_COMPARE1)) {
        nrf_timer_event_clear(esb_timer_inst, NRF_TIMER_EVENT_COMPARE1);
        nrf_timer_int_disable(esb_timer_inst, NRF_TIMER_INT_COMPARE1_MASK);
        nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_STOP);
        nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_CLEAR);
        retransmit_current();
    }
}

static void watchdog_handler(struct k_work *work) {
    if (esb_state == STATE_IDLE || esb_state == STATE_UNINIT) {
        return;
    }
    LOG_WRN("ESB watchdog: state %d stuck, RADIO=%u, resetting", esb_state, NRF_RADIO->STATE);
    on_radio_disabled = NULL;
    ppi_ack_timeout_disable();

    nrf_radio_shorts_set(NRF_RADIO, 0);
    nrf_radio_int_disable(NRF_RADIO, 0xFFFFFFFF);
    nrf_timer_task_trigger(esb_timer_inst, NRF_TIMER_TASK_STOP);
    nrf_timer_int_disable(esb_timer_inst, 0xFFFFFFFF);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    if (!WAIT_FOR(nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED), 100,
                  k_busy_wait(1))) {
        LOG_ERR("Watchdog: RADIO disable timed out");
    }
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    NVIC_ClearPendingIRQ(RADIO_IRQn);
    NVIC_ClearPendingIRQ(esb_timer_irqn);

    if (current_payload && esb_cfg.mode == ZMK_ESB_MODE_PTX) {
        last_tx_attempts = esb_cfg.retransmit_count + 1;
        signal_event(ZMK_ESB_EVENT_TX_FAILED);
        tx_fifo_remove_first();
    }
    esb_state = STATE_IDLE;
}

static void watchdog_start(void) { k_work_reschedule(&watchdog_work, K_MSEC(watchdog_timeout_ms)); }

static void watchdog_cancel(void) { k_work_cancel_delayable(&watchdog_work); }

static void radio_clear(void) {
    nrf_radio_shorts_set(NRF_RADIO, 0);
    nrf_radio_int_disable(NRF_RADIO, 0xFFFFFFFF);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    (void)WAIT_FOR(nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED), 100, k_busy_wait(1));
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
}

int zmk_esb_init(const struct zmk_esb_config *config) {
    if (!config || !config->event_handler) {
        return -EINVAL;
    }

    if (esb_state != STATE_UNINIT) {
        zmk_esb_disable();
    }

    memcpy(&esb_cfg, config, sizeof(esb_cfg));
    evt_handler = config->event_handler;

    if (esb_cfg.retransmit_delay > 0 && esb_cfg.retransmit_delay < RETRANSMIT_DELAY_MIN) {
        LOG_ERR("retransmit_delay %u < min %u", esb_cfg.retransmit_delay, RETRANSMIT_DELAY_MIN);
        return -EINVAL;
    }

    ack_timeout_us =
        (esb_cfg.bitrate == ZMK_ESB_BITRATE_2MBPS) ? ACK_TIMEOUT_2MBPS : ACK_TIMEOUT_1MBPS;

    uint32_t single_attempt_us = ESB_TX_TIME_US + ack_timeout_us + esb_cfg.retransmit_delay;
    watchdog_timeout_ms = ((esb_cfg.retransmit_count + 1) * single_attempt_us) / 1000 + 5;

    nrfx_err_t err = nrfx_ppi_channel_alloc(&ppi_ch_timer_disable);
    if (err != NRFX_SUCCESS) {
        LOG_ERR("PPI alloc failed: %d", err);
        return -ENOMEM;
    }

    esb_timer_inst = NRF_TIMER2;
    esb_timer_irqn = TIMER2_IRQn;

    int ret = hfclk_start();
    if (ret) {
        nrfx_ppi_channel_free(ppi_ch_timer_disable);
        return ret;
    }

    k_work_init(&evt_work, evt_work_handler);
    k_work_init_delayable(&watchdog_work, watchdog_handler);

    tx_fifo_reset();
    rx_fifo_reset();
    memset(rx_pipe_info, 0, sizeof(rx_pipe_info));
    memset(pids, 0, sizeof(pids));
    memset(ack_pl_pending, 0, sizeof(ack_pl_pending));

    timer_init();

    on_radio_disabled = NULL;
    radio_clear();

    // Clear residual radio events from BLE before configuring ESB
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PAYLOAD);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

    configure_radio();

    irq_disable(RADIO_IRQn);
    irq_disable(esb_timer_irqn);
    NVIC_ClearPendingIRQ(RADIO_IRQn);
    NVIC_ClearPendingIRQ(esb_timer_irqn);

    irq_connect_dynamic(RADIO_IRQn, ESB_RADIO_IRQ_PRIO, radio_isr, NULL, 0);
    irq_connect_dynamic(esb_timer_irqn, ESB_TIMER_IRQ_PRIO, timer_isr, NULL, 0);

    extern void _isr_wrapper(void);
#define VECTOR_TABLE_ENTRIES (16 + 48)
    static uint32_t ram_vector_table[VECTOR_TABLE_ENTRIES] __attribute__((aligned(256)));

    original_vtor = SCB->VTOR;
    memcpy(ram_vector_table, (uint32_t *)original_vtor, sizeof(ram_vector_table));
    ram_vector_table[RADIO_IRQn + 16] = (uint32_t)_isr_wrapper;
    SCB->VTOR = (uint32_t)ram_vector_table;
    __DSB();
    __ISB();

    irq_enable(esb_timer_irqn);

    esb_state = STATE_IDLE;

    LOG_INF("ESB initialized (mode=%s, bitrate=%s, ch=%u)",
            config->mode == ZMK_ESB_MODE_PTX ? "PTX" : "PRX",
            config->bitrate == ZMK_ESB_BITRATE_2MBPS ? "2M" : "1M", esb_addr.rf_channel);

    return 0;
}

void zmk_esb_disable(void) {
    if (esb_state == STATE_UNINIT) {
        return;
    }

    irq_disable(RADIO_IRQn);
    irq_disable(esb_timer_irqn);

    watchdog_cancel();
    on_radio_disabled = NULL;
    ppi_ack_timeout_disable();
    timer_deinit();
    radio_clear();

    nrfx_ppi_channel_free(ppi_ch_timer_disable);
    SCB->VTOR = original_vtor;
    __DSB();
    __ISB();

    esb_state = STATE_UNINIT;
    LOG_INF("ESB disabled");
}

bool zmk_esb_is_idle(void) { return esb_state == STATE_IDLE; }

int zmk_esb_write_payload(const struct zmk_esb_payload *payload) {
    if (esb_state == STATE_UNINIT) {
        return -EACCES;
    }
    if (!payload) {
        return -EINVAL;
    }
    int ret = hfclk_start();
    if (ret) {
        return ret;
    }
    if (payload->length == 0 || payload->length > CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH) {
        return -EMSGSIZE;
    }
    if (payload->pipe >= CONFIG_ZMK_ESB_PIPE_COUNT) {
        return -EINVAL;
    }

    if (esb_cfg.mode == ZMK_ESB_MODE_PTX) {
        unsigned int key = irq_lock();

        if (atomic_get(&tx_count) >= CONFIG_ZMK_ESB_TX_FIFO_SIZE) {
            irq_unlock(key);
            return -ENOMEM;
        }

        memcpy(&tx_fifo_buf[tx_back], payload, sizeof(struct zmk_esb_payload));

        pids[payload->pipe] = (pids[payload->pipe] + 1) % (PID_MAX + 1);
        tx_fifo_buf[tx_back].pid = pids[payload->pipe];

        if (++tx_back >= CONFIG_ZMK_ESB_TX_FIFO_SIZE) {
            tx_back = 0;
        }
        atomic_inc(&tx_count);

        if (esb_cfg.tx_mode == ZMK_ESB_TXMODE_AUTO && esb_state == STATE_IDLE) {
            start_tx_transaction();
        }
        irq_unlock(key);
    } else {
        unsigned int key = irq_lock();
        memcpy(&ack_pl[payload->pipe], payload, sizeof(struct zmk_esb_payload));
        ack_pl_pending[payload->pipe] = true;
        irq_unlock(key);
    }

    return 0;
}

int zmk_esb_read_rx_payload(struct zmk_esb_payload *payload) {
    if (esb_state == STATE_UNINIT) {
        return -EACCES;
    }
    if (!payload) {
        return -EINVAL;
    }

    unsigned int key = irq_lock();
    if (atomic_get(&rx_count) == 0) {
        irq_unlock(key);
        return -ENODATA;
    }

    memcpy(payload, &rx_fifo_buf[rx_front], sizeof(struct zmk_esb_payload));

    if (++rx_front >= CONFIG_ZMK_ESB_RX_FIFO_SIZE) {
        rx_front = 0;
    }
    atomic_dec(&rx_count);
    irq_unlock(key);

    return 0;
}

int zmk_esb_start_tx(void) {
    if (esb_state != STATE_IDLE) {
        return -EBUSY;
    }
    if (esb_cfg.mode != ZMK_ESB_MODE_PTX) {
        return -EPERM;
    }
    if (atomic_get(&tx_count) == 0) {
        return -ENODATA;
    }

    start_tx_transaction();
    return 0;
}

int zmk_esb_start_rx(void) {
    if (esb_state != STATE_IDLE) {
        return -EBUSY;
    }
    if (esb_cfg.mode == ZMK_ESB_MODE_PTX) {
        return -EPERM;
    }

    start_rx_listening();
    return 0;
}

int zmk_esb_stop_rx(void) {
    if (esb_cfg.mode == ZMK_ESB_MODE_PTX) {
        return -EPERM;
    }
    if (esb_state == STATE_IDLE || esb_state == STATE_UNINIT) {
        return 0;
    }

    on_radio_disabled = NULL;
    radio_clear();
    esb_state = STATE_IDLE;
    return 0;
}

int zmk_esb_flush_tx(void) {
    if (esb_state == STATE_UNINIT) {
        return -EACCES;
    }

    unsigned int key = irq_lock();
    if (esb_cfg.mode == ZMK_ESB_MODE_PTX && esb_state != STATE_IDLE && current_payload &&
        atomic_get(&tx_count) > 0) {
        // Keep the in-flight packet; only discard packets waiting behind it.
        tx_back = tx_front + 1;
        if (tx_back >= CONFIG_ZMK_ESB_TX_FIFO_SIZE) {
            tx_back = 0;
        }
        atomic_set(&tx_count, 1);
    } else {
        tx_fifo_reset();
    }
    memset(ack_pl_pending, 0, sizeof(ack_pl_pending));
    irq_unlock(key);
    return 0;
}

int zmk_esb_flush_rx(void) {
    if (esb_state == STATE_UNINIT) {
        return -EACCES;
    }
    unsigned int key = irq_lock();
    rx_fifo_reset();
    memset(rx_pipe_info, 0, sizeof(rx_pipe_info));
    irq_unlock(key);
    return 0;
}

int zmk_esb_set_base_address_0(const uint8_t *addr) {
    if (esb_state != STATE_IDLE && esb_state != STATE_UNINIT) {
        return -EBUSY;
    }
    if (!addr) {
        return -EINVAL;
    }
    memcpy(esb_addr.base_addr_p0, addr, sizeof(esb_addr.base_addr_p0));
    if (esb_state == STATE_IDLE) {
        nrf_radio_base0_set(NRF_RADIO, addr_conv(esb_addr.base_addr_p0));
    }
    return 0;
}

int zmk_esb_set_base_address_1(const uint8_t *addr) {
    if (esb_state != STATE_IDLE && esb_state != STATE_UNINIT) {
        return -EBUSY;
    }
    if (!addr) {
        return -EINVAL;
    }
    memcpy(esb_addr.base_addr_p1, addr, sizeof(esb_addr.base_addr_p1));
    if (esb_state == STATE_IDLE) {
        nrf_radio_base1_set(NRF_RADIO, addr_conv(esb_addr.base_addr_p1));
    }
    return 0;
}

int zmk_esb_set_prefixes(const uint8_t *prefixes, uint8_t num_pipes) {
    if (esb_state != STATE_IDLE && esb_state != STATE_UNINIT) {
        return -EBUSY;
    }
    if (!prefixes || num_pipes > CONFIG_ZMK_ESB_PIPE_COUNT) {
        return -EINVAL;
    }
    memcpy(esb_addr.pipe_prefixes, prefixes, num_pipes);
    esb_addr.num_pipes = num_pipes;
    esb_addr.rx_pipes_enabled = (uint8_t)(BIT(num_pipes) - 1);
    if (esb_state == STATE_IDLE) {
        nrf_radio_prefix0_set(NRF_RADIO, bytewise_bit_swap(&esb_addr.pipe_prefixes[0]));
        if (num_pipes > 4) {
            nrf_radio_prefix1_set(NRF_RADIO, bytewise_bit_swap(&esb_addr.pipe_prefixes[4]));
        }
    }
    return 0;
}

int zmk_esb_set_rf_channel(uint32_t channel) {
    if (channel > 100) {
        return -EINVAL;
    }
    esb_addr.rf_channel = channel;
    if (esb_state == STATE_IDLE) {
        nrf_radio_frequency_set(NRF_RADIO, RADIO_BASE_FREQ + channel);
    }
    return 0;
}

int zmk_esb_set_tx_power(int8_t tx_power_dbm_val) {
    if (esb_state != STATE_IDLE && esb_state != STATE_UNINIT) {
        return -EBUSY;
    }

    int8_t best = valid_tx_powers[0];
    int best_diff = 127;
    for (int i = 0; i < (int)ARRAY_SIZE(valid_tx_powers); i++) {
        int diff = (tx_power_dbm_val > valid_tx_powers[i])
                       ? (tx_power_dbm_val - valid_tx_powers[i])
                       : (valid_tx_powers[i] - tx_power_dbm_val);
        if (diff < best_diff) {
            best_diff = diff;
            best = valid_tx_powers[i];
        }
    }
    if (best != tx_power_dbm_val) {
        LOG_WRN("TX power %d dBm not supported, using %d dBm", tx_power_dbm_val, best);
    }

    tx_power_dbm = best;
    if (esb_state == STATE_IDLE) {
        nrf_radio_txpower_set(NRF_RADIO, (nrf_radio_txpower_t)tx_power_dbm);
    }
    return 0;
}
