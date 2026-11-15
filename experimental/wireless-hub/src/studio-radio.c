#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/random/random.h>
#include <zmk/esb.h>
#include <zmk/2g4.h>
#include <zmk/2g4_crypto.h>
#include "studio-radio.h"
#include "studio-radio-link.h"
#include "hub-peer.h"
#if IS_ENABLED(CONFIG_ZMK_STUDIO)
#include <zmk/studio/rpc.h>
#include <zmk/studio/core.h>
#endif

struct radio_message { uint8_t peer,len, data[20]; };
K_MSGQ_DEFINE(studio_packets,sizeof(struct radio_message),8,4);
static struct sr_link link;
static bool active, synchronized;
static int64_t last_rx;
static atomic_t ready;
static atomic_t session_open;
static unsigned session_peer;
bool studio_radio_busy(void) {
    const struct device *dev=DEVICE_DT_GET(DT_NODELABEL(studio_radio_uart));
    uint32_t dtr=0;
    if(!device_is_ready(dev))return true;
    uart_line_ctrl_get(dev,UART_LINE_CTRL_DTR,&dtr);
    return dtr || atomic_get(&session_open);
}
void studio_radio_status(char *out,size_t size) {
    snprintk(out,size,"STUDIO_2G4=1\r\nSTUDIO_ACTIVE=%d\r\nSTUDIO_PORT=SECOND_CDC\r\nBLE_MOUSE=1\r\n",active && synchronized);
}

void studio_radio_started(void) { atomic_set(&ready,1); }
void studio_radio_received(const uint8_t *data,size_t len) {
    studio_radio_received_from(hub_peer_target(),data,len);
}
void studio_radio_received_from(unsigned slot,const uint8_t *data,size_t len) {
    if(slot!=hub_peer_target())return;
    if(!sr_valid(data,len))return;
    struct radio_message m={.peer=slot,.len=len};memcpy(m.data,data,len);
    // Overflow is recovered by stop-and-wait retransmission with a fresh CCM counter.
    k_msgq_put(&studio_packets,&m,K_NO_WAIT);
}

#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
static const struct device *const uart=DEVICE_DT_GET(DT_NODELABEL(studio_radio_uart));
RING_BUF_DECLARE(host_rx,2048);
RING_BUF_DECLARE(host_tx,2048);
static bool was_dtr;
static uint16_t connection_count;
static void serial_irq(const struct device *dev,void *arg) {
    if(!uart_irq_update(dev))return;
    uint32_t dtr=0;uart_line_ctrl_get(dev,UART_LINE_CTRL_DTR,&dtr);
    if(!dtr){uart_irq_rx_disable(dev);uart_irq_tx_disable(dev);return;}
    if(uart_irq_rx_ready(dev)) {
        uint8_t b[32];int n;
        while(ring_buf_space_get(&host_rx)>=sizeof(b) && (n=uart_fifo_read(dev,b,sizeof(b)))>0)
            ring_buf_put(&host_rx,b,n);
        if(ring_buf_space_get(&host_rx)<sizeof(b))uart_irq_rx_disable(dev);
    }
    if(uart_irq_tx_ready(dev)) {
        uint8_t *b;uint32_t n=ring_buf_get_claim(&host_tx,&b,64);
        int sent=n ? uart_fifo_fill(dev,b,n):0;
        ring_buf_get_finish(&host_tx,sent>0?sent:0);
        // A full CDC FIFO must yield its shared workqueue. Otherwise repeated
        // TX-ready callbacks can starve USB control and DTR-close handling.
        if(sent<=0 || ring_buf_is_empty(&host_tx))uart_irq_tx_disable(dev);
    }
}
static void new_host_session(void) {
    // The crypto boot epoch advances in NVS; the low word identifies host opens.
    // Refuse reuse if a single boot exceeds 65535 serial sessions.
    if(connection_count==UINT16_MAX){active=false;return;}
    uint32_t session=(zmk_2g4_crypto_tx_session_id()<<16)|++connection_count;
    sr_reset(&link,session);synchronized=false;active=true;last_rx=k_uptime_get();
}
static struct ring_buf *input(void){return &host_rx;}
static struct ring_buf *output(void){return &host_tx;}
static bool can_receive(void){return true;}
static void received_bytes(void){uart_irq_tx_enable(uart);}
#else
static atomic_t studio_selected;
static int rx_start(void){atomic_set(&studio_selected,1);return 0;}
static int rx_stop(void){atomic_set(&studio_selected,0);return 0;}
static void tx_notify(struct ring_buf *buf,size_t added,bool done,void *arg){}
ZMK_RPC_TRANSPORT(radio,ZMK_TRANSPORT_2G4,rx_start,rx_stop,NULL,tx_notify);
static struct ring_buf *input(void){return zmk_rpc_get_tx_buf();}
static struct ring_buf *output(void){return zmk_rpc_get_rx_buf();}
static bool can_receive(void){return atomic_get(&studio_selected);}
static void received_bytes(void){zmk_rpc_rx_notify();}
static void abort_rpc_stream(void) {
    // EOF terminates any partially decoded request; the next SOF starts a new one.
    uint8_t eof=0xAD;ring_buf_put(output(),&eof,1);received_bytes();
    zmk_studio_core_lock();
}
#endif

static void emit(uint8_t flags) {
    struct zmk_esb_payload p={.pipe=session_peer,.noack=false};
    size_t n=sr_encode(&link,flags,p.data);
    int encrypted=hub_peer_encrypt(session_peer,p.data,n,sizeof(p.data));
    if(encrypted<=0)return;
    p.length=encrypted;zmk_esb_write_payload(&p);
}
static void process(struct radio_message *m) {
    if(m->peer!=session_peer)return;
    uint32_t session=sr_session(m->data);
#if !IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    if(!zmk_2g4_is_ready() || !can_receive())return;
    if((m->data[1]&SR_OPEN) && (!link.session || (int32_t)(session-link.session)>0)) {
        sr_reset(&link,session);active=true;synchronized=true;abort_rpc_stream();
        // Old responses belong to the previous serial connection.
        uint8_t drop[32];while(ring_buf_get(input(),drop,sizeof(drop))){}
    }
#endif
    if(!active || session!=link.session)return;
    last_rx=k_uptime_get();synchronized=true;
    if(m->data[1]&SR_CLOSE) {
        active=false;
#if !IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
        abort_rpc_stream();
#endif
        return;
    }
    sr_ack(&link,m->data);
    size_t len=m->len-SR_HEADER;
    if(can_receive() && sr_data_ready(&link,m->data,m->len) && ring_buf_space_get(output())>=len) {
        ring_buf_put(output(),m->data+SR_HEADER,len);link.rx++;received_bytes();
    }
}
static void radio_main(void) {
#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
    if(!device_is_ready(uart))return;
    uart_irq_callback_user_data_set(uart,serial_irq,NULL);
#endif
    for(;;) {
        if(!atomic_get(&ready)){k_msleep(10);continue;}
#if IS_ENABLED(CONFIG_ZMK_2G4_DONGLE)
        uint32_t dtr=0;uart_line_ctrl_get(uart,UART_LINE_CTRL_DTR,&dtr);
        uint32_t baud=0;uart_line_ctrl_get(uart,UART_LINE_CTRL_BAUD_RATE,&baud);
        unsigned requested=baud==230400?1:baud==115200?0:hub_peer_target();
        if(dtr && was_dtr && requested!=session_peer) {
            if(active)emit(SR_CLOSE);
            active=false;was_dtr=false;atomic_clear(&session_open);
        }
        if(dtr && !was_dtr) {
            atomic_set(&session_open,1);
            uart_irq_rx_disable(uart);uart_irq_tx_disable(uart);
            // Serial client receives a fresh byte stream per DTR session.
            ring_buf_reset(&host_rx);ring_buf_reset(&host_tx);session_peer=requested;
            bool switched=requested!=hub_peer_target();
            if(!hub_peer_select_session(requested) && (!switched || !hub_radio_reconfigure())) {
                new_host_session();
                // Slot 0 preserves the legacy binary-only stream. Slot 1 requires
                // explicit target confirmation before a client sends any RPC.
                if(requested) {
                    const char *banner="HUB_TARGET=1\r\n";
                    ring_buf_put(&host_tx,(const uint8_t *)banner,14);uart_irq_tx_enable(uart);
                }
                uart_irq_rx_enable(uart);
            } else active=false;
        }else if(!dtr && was_dtr) {
            if(active)emit(SR_CLOSE);
            active=false;uart_irq_rx_disable(uart);uart_irq_tx_disable(uart);
            atomic_clear(&session_open);
        }
        was_dtr=dtr;
        if(active && k_uptime_get()-last_rx>3000)new_host_session();
        if(dtr && active && ring_buf_space_get(&host_rx)>=32)uart_irq_rx_enable(uart);
        if(dtr && active && !ring_buf_is_empty(&host_tx))uart_irq_tx_enable(uart);
#else
        if(!zmk_2g4_is_ready() || !can_receive())active=false;
        if(active && k_uptime_get()-last_rx>3000){active=false;abort_rpc_stream();}
        // Keep RPC serialization from blocking endpoint changes after disconnect.
        if(!active) {uint8_t drop[32];ring_buf_get(input(),drop,sizeof(drop));}
#endif
        struct radio_message message;
        while(k_msgq_get(&studio_packets,&message,K_NO_WAIT)==0)process(&message);
        if(active) {
            if(synchronized && !link.pending_len) {
                link.pending_len=ring_buf_get(input(),link.pending,SR_DATA);
            }
            emit(synchronized?0:SR_OPEN);
        }
        k_msleep(active?5:20);
    }
}
K_THREAD_DEFINE(studio_radio_thread,2048,radio_main,NULL,NULL,NULL,5,0,50);
