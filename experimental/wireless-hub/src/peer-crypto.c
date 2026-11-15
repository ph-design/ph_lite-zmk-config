/* SPDX-License-Identifier: MIT */
#include "peer-crypto.h"
#include <errno.h>
#include <string.h>
#include <tinycrypt/aes.h>
#include <tinycrypt/ccm_mode.h>
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=v>>(8*i);}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int ccm(bool encrypt,const uint8_t *key,uint32_t session,uint32_t ctr,uint8_t dir,
               const uint8_t *in,size_t len,uint8_t *out) {
    uint8_t nonce[13]={0},aad=0;
    put32(nonce,ctr);nonce[4]=(dir&1)<<7;put32(nonce+5,session);
    struct tc_aes_key_sched_struct schedule;
    struct tc_ccm_mode_struct mode;
    if(!tc_aes128_set_encrypt_key(&schedule,key) || !tc_ccm_config(&mode,&schedule,nonce,13,4))return -EIO;
    int ok=encrypt?tc_ccm_generation_encryption(out,len+4,&aad,1,in,len,&mode)
                  :tc_ccm_decryption_verification(out,len,&aad,1,in,len+4,&mode);
    memset(&schedule,0,sizeof(schedule));
    return ok?0:-EBADMSG;
}
int peer_encrypt(struct peer_crypto *s,uint8_t dir,uint8_t *data,size_t len,size_t cap) {
    if(!s || !data || !len || len>20 || cap<len+12)return -EINVAL;
    if(s->tx_counter==UINT32_MAX)return -ERANGE;
    uint32_t ctr=s->tx_counter++;
    uint8_t out[24];int err=ccm(true,s->key,s->tx_session,ctr,dir,data,len,out);
    if(err)return err;
    put32(data,s->tx_session);put32(data+4,ctr);memcpy(data+8,out,len+4);
    return len+12;
}
int peer_decrypt(struct peer_crypto *s,uint8_t dir,uint8_t *data,size_t len) {
    if(!s || !data || len<=12 || len>32)return -EINVAL;
    uint32_t session=get32(data),ctr=get32(data+4);uint8_t out[20];
    int err=ccm(false,s->key,session,ctr,dir,data+8,len-12,out);if(err)return err;
    if(s->rx_initialized && session!=s->rx_session && (int32_t)(session-s->rx_session)<=0)return -EACCES;
    if(!s->rx_initialized || session!=s->rx_session) {
        s->rx_initialized=true;s->rx_session=session;s->rx_counter=ctr;s->rx_window=1;
    } else if(ctr>s->rx_counter) {
        uint32_t shift=ctr-s->rx_counter;
        s->rx_window=shift>=64?1:(s->rx_window<<shift)|1;s->rx_counter=ctr;
    } else {
        uint32_t diff=s->rx_counter-ctr;
        if(diff>=64 || (s->rx_window&(1ULL<<diff)))return -EACCES;
        s->rx_window|=1ULL<<diff;
    }
    memcpy(data,out,len-12);return len-12;
}
