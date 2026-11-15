"""Execute the actual UART callback with a full FIFO and multi-packet replies."""
from pathlib import Path
import re, subprocess, sys, tempfile
source=(Path(sys.argv[1])/"app/src/studio/uart_rpc_transport.c").read_text()
def function(name):
    a=re.search(r'^static (?:int|void) '+name+r'\(',source,re.M).start()
    b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
with tempfile.TemporaryDirectory(prefix='uart-runtime-') as d:
    p=Path(d)
    code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define CONFIG_UART_INTERRUPT_DRIVEN 1
#define IS_ENABLED(x) (x)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_ERR(...)
struct device{int unused;};static struct device device;static const struct device*uart_dev=&device;
struct k_spinlock{bool held;};typedef int k_spinlock_key_t;
static struct k_spinlock uart_state_lock;static bool uart_selected;
static int k_spin_lock(struct k_spinlock*l){assert(!l->held);l->held=true;return 0;}
static void k_spin_unlock(struct k_spinlock*l,int key){assert(l->held);l->held=false;}
struct ring_buf{uint8_t data[4096];uint32_t size,tail;};
static struct ring_buf tx,rx;
static struct ring_buf*zmk_rpc_get_tx_buf(void){return &tx;}
static struct ring_buf*zmk_rpc_get_rx_buf(void){return &rx;}
static uint32_t ring_buf_get_claim(struct ring_buf*b,uint8_t**p,uint32_t n){*p=b->data+b->tail;return MIN(n,b->size-b->tail);}
static void ring_buf_get_finish(struct ring_buf*b,uint32_t n){b->tail+=n;assert(b->tail<=b->size);}
static uint32_t ring_buf_put_claim(struct ring_buf*b,uint8_t**p,uint32_t n){*p=b->data;return 0;}
static void ring_buf_put_finish(struct ring_buf*b,uint32_t n){}
static bool ring_buf_is_empty(struct ring_buf*b){return b->tail==b->size;}
static void zmk_rpc_rx_notify(void){}
static bool tx_irq,rx_irq,ready=true;static int allowance=64;static unsigned fill_calls,output_len;
static uint8_t output[4096];
static bool uart_irq_update(const struct device*d){return true;}
static bool uart_irq_tx_ready(const struct device*d){return ready&&tx_irq;}
static bool uart_irq_rx_ready(const struct device*d){return false;}
static void uart_irq_tx_disable(const struct device*d){tx_irq=false;}
static void uart_irq_rx_disable(const struct device*d){rx_irq=false;}
static void uart_irq_rx_enable(const struct device*d){rx_irq=true;}
static int uart_fifo_read(const struct device*d,uint8_t*p,int n){return 0;}
static int uart_fifo_fill(const struct device*d,uint8_t*p,int n){
 assert(++fill_calls<1000);ready=false;
 if(allowance<0)return allowance;
 n=MIN(n,allowance);memcpy(output+output_len,p,n);output_len+=n;return n;
}
'''
    code+='\n'+function('start_rx')+'\n'+function('stop_rx')+'\n'+function('serial_cb')
    code+=r'''
int main(void){
 start_rx();assert(rx_irq&&uart_selected);tx_irq=true;
 tx.size=3000;for(unsigned i=0;i<tx.size;i++)tx.data[i]=(i*37)&255;
 allowance=0;serial_cb(0,0);assert(fill_calls==1&&tx.tail==0&&!uart_state_lock.held&&tx_irq);
 /* Completion runs between callbacks; a full FIFO never blocks that work. */
 allowance=17;ready=true;serial_cb(0,0);assert(tx.tail==17&&output_len==17&&fill_calls==2);
 allowance=-5;ready=true;serial_cb(0,0);assert(tx.tail==17&&fill_calls==3);
 allowance=64;
 while(!ring_buf_is_empty(&tx)){unsigned old=fill_calls;ready=true;serial_cb(0,0);assert(fill_calls==old+1);}
 assert(!tx_irq&&output_len==3000&&!memcmp(output,tx.data,3000));
 /* A late USB callback after switching to radio cannot take radio data. */
 stop_rx();assert(!rx_irq&&!tx_irq&&!uart_selected&&!uart_state_lock.held);
 tx.tail=0;tx_irq=true;ready=true;unsigned old=fill_calls;serial_cb(0,0);
 assert(fill_calls==old&&tx.tail==0&&!tx_irq&&!uart_state_lock.held);
 start_rx();tx_irq=true;ready=true;serial_cb(0,0);assert(tx.tail==64);
 puts("PASS: actual USB UART full/partial/error FIFO, 3000-byte reply integrity, bounded callbacks, stop/late callback and resume");
}
'''
    (p/'test.c').write_text(code)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Wno-unused-parameter','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=5)
