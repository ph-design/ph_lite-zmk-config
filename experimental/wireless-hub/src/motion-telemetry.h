/* SPDX-License-Identifier: MIT
 * Optional authenticated keepalive extension. No input semantics change.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MOTION_STATUS_TTL_MS 1250u
enum motion_status { MOTION_UNKNOWN=0, MOTION_READY=1, MOTION_LOCKED=2 };
static inline void motion_status_encode(uint8_t out[4], uint8_t battery, bool locked) {
    out[0]=0xfe; out[1]=battery; out[2]=0xa1;
    out[3]=locked ? MOTION_LOCKED : MOTION_READY;
}
static inline enum motion_status motion_status_decode(const uint8_t *p, size_t size) {
    if(size!=4 || p[0]!=0xfe || p[2]!=0xa1 ||
       (p[3]!=MOTION_READY && p[3]!=MOTION_LOCKED)) return MOTION_UNKNOWN;
    return (enum motion_status)p[3];
}
static inline enum motion_status motion_status_fresh(unsigned state, uint32_t age, bool connected) {
    return connected && age<=MOTION_STATUS_TTL_MS &&
           (state==MOTION_READY || state==MOTION_LOCKED) ? (enum motion_status)state : MOTION_UNKNOWN;
}
static inline const char *motion_status_name(enum motion_status state) {
    return state==MOTION_READY ? "READY" : state==MOTION_LOCKED ? "LOCKED" : "UNKNOWN";
}
