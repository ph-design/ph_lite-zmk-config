"""Compile the actual patched heartbeat function with radio/hardware mocks."""
from pathlib import Path
import subprocess
import sys
import tempfile
root=Path(sys.argv[1])
text=(root/'app/src/2g4.c').read_text()
start=text.index('static void keepalive_handler(')
body=text[start:text.index('/* A state transition gets',start)]
with tempfile.TemporaryDirectory(prefix='motion-sender-') as directory:
    temp=Path(directory)
    harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include "motion-telemetry.h"
#define CONFIG_ZMK_2G4_MAX_CONSEC_FAILURES 5
#define CONFIG_ZMK_ESB_MAX_PAYLOAD_LENGTH 32
#define ZMK_2G4_KEEPALIVE_MS 250
#define ZMK_2G4_MSG_KEEP_ALIVE 0xfe
#define K_MSEC(x) (x)
struct k_work { int unused; };
static int keepalive_work;
static struct { unsigned pipe,length;bool noack;uint8_t data[32]; } tx_payload;
static bool ready,locked;
static uint8_t consec_fail_count;
static int transmissions,delay_ms,encrypted_length;
static bool encrypt_fails;
static bool zmk_battery_is_valid(void){return true;}
static uint8_t zmk_battery_state_of_charge(void){return 75;}
static bool zmk_motion_guard_blocks_output(void){return locked;}
static int zmk_2g4_crypto_encrypt(uint8_t *p,int n,int cap){
    assert(cap==32 && n==4 && p[0]==0xfe && p[1]==75);
    encrypted_length=n;return encrypt_fails ? -1 : n+8;
}
static void zmk_esb_write_payload(void *p){(void)p;transmissions++;}
static void k_work_reschedule(void *w,int ms){(void)w;delay_ms=ms;}
'''+body+r'''
int main(void){
    keepalive_handler(0);assert(transmissions==0);
    ready=true;locked=true;
    keepalive_handler(0);
    assert(transmissions==1 && delay_ms==250 && encrypted_length==4);
    assert(motion_status_decode(tx_payload.data,4)==MOTION_LOCKED);
    locked=false;keepalive_handler(0);
    assert(transmissions==2 && motion_status_decode(tx_payload.data,4)==MOTION_READY);
    consec_fail_count=5;locked=true;keepalive_handler(0);
    assert(transmissions==3 && delay_ms==1000);
    assert(motion_status_decode(tx_payload.data,4)==MOTION_LOCKED);
    encrypt_fails=true;keepalive_handler(0);
    assert(transmissions==3);
    puts("PASS: actual heartbeat locked/ready encoding, encryption, idle cadence, disconnected locked retry probe");
}
'''
    (temp/'case.c').write_text(harness)
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-fsanitize=address,undefined','-I',str(root/'app/include/zmk'),
                    str(temp/'case.c'),'-o',str(temp/'case')],check=True)
    subprocess.run([str(temp/'case')],check=True)
