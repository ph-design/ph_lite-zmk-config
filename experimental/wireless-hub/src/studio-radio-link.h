#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// 32-byte ESB frame - 12-byte authenticated encryption overhead - 8-byte header.
#define SR_TYPE 0x40
#define SR_OPEN 1
#define SR_CLOSE 2
#define SR_HEADER 8
#define SR_DATA 12
struct sr_link {
    uint32_t session;
    uint8_t tx, rx, pending[SR_DATA], pending_len;
};
static inline uint32_t sr_session(const uint8_t *p) {
    return (uint32_t)p[2] | (uint32_t)p[3]<<8 | (uint32_t)p[4]<<16 | (uint32_t)p[5]<<24;
}
static inline void sr_reset(struct sr_link *s, uint32_t session) {
    memset(s,0,sizeof(*s)); s->session=session;
}
static inline bool sr_valid(const uint8_t *p,size_t len) {
    return len>=SR_HEADER && len<=SR_HEADER+SR_DATA && p[0]==SR_TYPE && !(p[1]&~(SR_OPEN|SR_CLOSE));
}
static inline size_t sr_encode(const struct sr_link *s,uint8_t flags,uint8_t *p) {
    p[0]=SR_TYPE;p[1]=flags;
    for(int i=0;i<4;i++)p[2+i]=(uint8_t)(s->session>>(8*i));
    p[6]=s->tx;p[7]=s->rx;
    memcpy(p+SR_HEADER,s->pending,s->pending_len);
    return SR_HEADER+s->pending_len;
}
// Acknowledge only the single outstanding fragment; sequence rollover is intentional.
static inline void sr_ack(struct sr_link *s,const uint8_t *p) {
    if(s->pending_len && p[7]==(uint8_t)(s->tx+1)) {s->pending_len=0;s->tx++;}
}
static inline bool sr_data_ready(const struct sr_link *s,const uint8_t *p,size_t len) {
    return len>SR_HEADER && p[6]==s->rx;
}
static inline void sr_stage(struct sr_link *s,const uint8_t *p,size_t len) {
    if(len<=SR_DATA && !s->pending_len) {memcpy(s->pending,p,len);s->pending_len=(uint8_t)len;}
}
