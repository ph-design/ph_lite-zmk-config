"""Execute the actual patched serializer/transport selector with a stalled TX ring."""
from pathlib import Path
import subprocess, sys, tempfile
source=(Path(sys.argv[1])/"app/src/studio/rpc.c").read_text()
core_source=(Path(sys.argv[1])/"app/src/studio/core_subsystem.c").read_text()
def function(name):
    at=source.index(name+'(');a=source.rfind('\n',0,at)+1;b=source.index('{',at)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
with tempfile.TemporaryDirectory(prefix='studio-runtime-') as d:
    p=Path(d)
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define K_NO_WAIT 0
#define K_FOREVER -1
#define LOG_WRN(...)
#define IS_ENABLED(x) 1
#define CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT 1
#define FRAMING_SOF 0xAB
#define FRAMING_EOF 0xAD
#define FRAMING_ESC 0xAC
#define zmk_studio_Response_notification_tag 2
#define zmk_studio_Notification_motion_tag 8
struct ring_buf {uint8_t data[256];uint32_t head,tail;};
static struct ring_buf rpc_tx_buf;
static uint32_t ring_buf_size_get(struct ring_buf*b){return b->head-b->tail;}
static uint32_t ring_buf_capacity_get(struct ring_buf*b){return sizeof(b->data);}
static uint32_t ring_buf_space_get(struct ring_buf*b){return sizeof(b->data)-ring_buf_size_get(b);}
static uint32_t ring_buf_put_claim(struct ring_buf*b,uint8_t**data,uint32_t n){uint32_t space=ring_buf_space_get(b),room=sizeof(b->data)-b->head%sizeof(b->data);if(n>space)n=space;if(n>room)n=room;*data=b->data+b->head%sizeof(b->data);return n;}
static void ring_buf_put_finish(struct ring_buf*b,uint32_t n){b->head+=n;}
static uint32_t ring_buf_put(struct ring_buf*b,const uint8_t*d,uint32_t n){uint32_t i=0;while(i<n&&ring_buf_space_get(b)){b->data[b->head++%sizeof(b->data)]=d[i++];}return i;}
static uint32_t ring_buf_get(struct ring_buf*b,uint8_t*d,uint32_t n){uint32_t i=0;while(i<n&&ring_buf_size_get(b)){d[i++]=b->data[b->tail++%sizeof(b->data)];}return i;}
typedef struct pb_ostream_s pb_ostream_t;
struct pb_ostream_s {bool(*callback)(pb_ostream_t*,const uint8_t*,size_t);void*state;size_t max_size,bytes_written;};
typedef struct {bool accepted;int which_type;struct {struct {int which_subsystem;} notification;} type;} zmk_studio_Response;
static uint8_t payload[300];static size_t payload_len;static bool encode_error;
static int zmk_studio_Response_msg;
static bool pb_encode(pb_ostream_t*s,const void*desc,const zmk_studio_Response*r){if(encode_error)return false;bool ok=s->callback(s,payload,payload_len);s->bytes_written+=payload_len;return ok;}
static int rpc_transport_mutex;static int64_t now;static bool release_on_sleep;
static int k_mutex_lock(int*m,int wait){if(*m)return -EBUSY;*m=1;return 0;}
static void k_mutex_unlock(int*m){assert(*m);*m=0;}
static int64_t k_uptime_get(void){return now;}
static void k_msleep(int ms){now+=ms;if(release_on_sleep)rpc_tx_buf.tail=rpc_tx_buf.head;}
enum zmk_transport{ZMK_TRANSPORT_USB,ZMK_TRANSPORT_2G4};
static bool usb_ready;static enum zmk_transport hid_transport=ZMK_TRANSPORT_2G4;
struct endpoint{enum zmk_transport transport;};
static struct endpoint zmk_endpoints_selected(void){return (struct endpoint){hid_transport};}
static bool zmk_usb_is_hid_ready(void){return usb_ready;}
static unsigned notices,starts,stops,locks;
static void tx_notify(struct ring_buf*b,size_t n,bool done,void*arg){notices++;}
static int rx_start(void){starts++;return 0;}
static int rx_stop(void){stops++;return 0;}
static void zmk_studio_core_lock(void){locks++;}
struct zmk_rpc_transport{enum zmk_transport transport;int(*rx_start)(void);int(*rx_stop)(void);void*(*tx_user_data)(void);void(*tx_notify)(struct ring_buf*,size_t,bool,void*);};
static struct zmk_rpc_transport transports[2]={{ZMK_TRANSPORT_USB,rx_start,rx_stop,0,tx_notify},{ZMK_TRANSPORT_2G4,rx_start,rx_stop,0,tx_notify}};
static struct zmk_rpc_transport *selected_transport=transports+1;
#define STRUCT_SECTION_FOREACH(type,t) for(struct type *t=transports;t<transports+2;t++)
#define LOG_WRN(...)
'''
    for name in ['rpc_tx_buffer_write','pb_ostream_for_tx_buf','escaped_size','send_response','zmk_rpc_radio_tx_read','refresh_selected_transport']:
        code+='\n'+function(name)+'\n'
    rpc_source=source;source=core_source
    code+=r'''
#define CONFIG_SOC_SERIES_NRF52X 1
#define RST_UF2 0x57
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define CORE_RESPONSE(type,value) ((zmk_studio_Response){.accepted=(value)})
struct k_work {int unused;};
static struct {unsigned GPREGRET;} power_registers;
#define NRF_POWER (&power_registers)
static unsigned resets,boot_delay;
static void __DSB(void){}
static void NVIC_SystemReset(void){resets++;}
static int bootloader_work;
static void k_work_reschedule(int*work,int delay){boot_delay=delay;}
typedef struct {struct {struct {struct {bool enter_bootloader;}request_type;}core;}subsystem;} zmk_studio_Request;
'''
    code+='\n'+function('bootloader_work_cb')+'\n'+function('enter_bootloader')
    source=rpc_source
    code+=r'''
int main(void){
 zmk_studio_Response live={.which_type=2,.type.notification.which_subsystem=8},reply={0};
 payload[0]=0;payload[1]=FRAMING_SOF;payload[2]=FRAMING_ESC;payload[3]=FRAMING_EOF;payload_len=4;
 /* Wrapped frames containing every escaped byte stay intact. */
 rpc_tx_buf.head=rpc_tx_buf.tail=254;assert(!send_response(&live));
 uint8_t out[256];unsigned n=ring_buf_get(&rpc_tx_buf,out,sizeof(out));
 const uint8_t expected[]={FRAMING_SOF,0,FRAMING_ESC,FRAMING_SOF,FRAMING_ESC,FRAMING_ESC,FRAMING_ESC,FRAMING_EOF,FRAMING_EOF};
 assert(n==sizeof(expected)&&!memcmp(out,expected,n)&&!rpc_transport_mutex);
 /* A stopped radio/host cannot spin or block keys or grow old telemetry. */
 rpc_tx_buf.head=rpc_tx_buf.tail+256;int64_t before=now;unsigned old=notices;
 for(unsigned i=0;i<10000;i++)assert(send_response(&live)==-EAGAIN);
 assert(now==before&&notices==old&&!rpc_transport_mutex&&ring_buf_size_get(&rpc_tx_buf)==256);
 assert(send_response(&reply)==-EAGAIN&&now-before==500&&!rpc_transport_mutex);
 release_on_sleep=true;assert(!send_response(&reply));release_on_sleep=false;
 /* Errors never retain the mutex or publish SOF. */
 rpc_tx_buf.tail=rpc_tx_buf.head;encode_error=true;old=notices;
 assert(send_response(&reply)==-EINVAL&&!rpc_transport_mutex&&notices==old);encode_error=false;
 payload_len=300;assert(send_response(&reply)==-EMSGSIZE&&!rpc_transport_mutex);payload_len=4;
 assert(!send_response(&reply));
 /* Switching USB management while HID remains 2.4G cannot lose USB bytes. */
 usb_ready=true;refresh_selected_transport();assert(selected_transport==transports&&hid_transport==ZMK_TRANSPORT_2G4&&starts==1&&stops==1&&locks==1);
 n=ring_buf_size_get(&rpc_tx_buf);assert(zmk_rpc_radio_tx_read(out,256)==0&&ring_buf_size_get(&rpc_tx_buf)==n);
 usb_ready=false;refresh_selected_transport();assert(selected_transport==transports+1);
 assert(zmk_rpc_radio_tx_read(out,256)==n);
 rpc_transport_mutex=1;assert(zmk_rpc_radio_tx_read(out,256)==0);rpc_transport_mutex=0;
 zmk_studio_Request req={.subsystem.core.request_type.enter_bootloader=true};
 usb_ready=false;assert(!enter_bootloader(&req).accepted&&!boot_delay&&!resets);
 usb_ready=true;req.subsystem.core.request_type.enter_bootloader=false;assert(!enter_bootloader(&req).accepted&&!boot_delay);
 req.subsystem.core.request_type.enter_bootloader=true;assert(enter_bootloader(&req).accepted&&boot_delay==300&&!resets);
 bootloader_work_cb(0);assert(resets==1&&power_registers.GPREGRET==RST_UF2);
 puts("PASS: USB-only bootloader acceptance, delayed reset and UF2 magic");
 puts("PASS: actual escaped frames, 10000 stalled notifications, bounded request wait, encoder failure/oversize unlock, USB priority and radio ownership");
}
'''
    (p/'test.c').write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
