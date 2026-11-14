/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "motion-telemetry.h"
#include "led-motion.h"
int main(void) {
    uint8_t p[4];motion_status_encode(p,75,true);
    assert(p[0]==0xfe && p[1]==75 && motion_status_decode(p,4)==MOTION_LOCKED);
    motion_status_encode(p,255,false);
    assert(p[1]==255 && motion_status_decode(p,4)==MOTION_READY);
    uint8_t legacy[]={0xfe,80};
    assert(motion_status_decode(legacy,2)==MOTION_UNKNOWN);
    assert(motion_status_decode(legacy,1)==MOTION_UNKNOWN);
    assert(motion_status_decode(NULL,0)==MOTION_UNKNOWN);
    motion_status_encode(p,50,true);p[2]=0;
    assert(motion_status_decode(p,4)==MOTION_UNKNOWN);
    p[2]=0xa1;p[3]=3;assert(motion_status_decode(p,4)==MOTION_UNKNOWN);
    p[3]=2;p[0]=1;assert(motion_status_decode(p,4)==MOTION_UNKNOWN);
    assert(motion_status_fresh(MOTION_LOCKED,1250,true)==MOTION_LOCKED);
    assert(motion_status_fresh(MOTION_LOCKED,1251,true)==MOTION_UNKNOWN);
    assert(motion_status_fresh(MOTION_LOCKED,0,false)==MOTION_UNKNOWN);
    assert(motion_status_fresh(MOTION_UNKNOWN,0,true)==MOTION_UNKNOWN);
    assert(!strcmp(motion_status_name(MOTION_READY),"READY"));

    assert(led_motion_select(0,0)==LED_MOTION_NORMAL);
    assert(led_motion_select(1,0)==LED_MOTION_NORMAL);
    assert(led_motion_select(1,1)==LED_MOTION_ALL_LOCKED);
    assert(led_motion_select(2,2)==LED_MOTION_ALL_LOCKED);
    assert(led_motion_select(3,3)==LED_MOTION_ALL_LOCKED);
    assert(led_motion_select(3,1)==LED_MOTION_MIXED);
    assert(led_motion_select(3,2)==LED_MOTION_MIXED);
    assert(led_motion_select(3,0)==LED_MOTION_NORMAL);
    assert(led_motion_select(1,2)==LED_MOTION_NORMAL); /* offline stale peer ignored */
    /* A legacy/expired peer stays UNKNOWN; never count it as locked. */
    unsigned locked=0;
    if(motion_status_fresh(MOTION_LOCKED,10,true)==MOTION_LOCKED)locked|=1;
    if(motion_status_fresh(MOTION_LOCKED,1300,true)==MOTION_LOCKED)locked|=2;
    assert(led_motion_select(3,locked)==LED_MOTION_MIXED);

    assert(led_motion_level(LED_MOTION_ALL_LOCKED,0)==5);
    assert(led_motion_level(LED_MOTION_ALL_LOCKED,1200)==100);
    assert(led_motion_level(LED_MOTION_MIXED,0)==65);
    assert(led_motion_level(LED_MOTION_MIXED,1200)==100);
    unsigned previous=0;
    for(unsigned ms=0;ms<=1200;ms++) {
        unsigned a=led_motion_level(LED_MOTION_ALL_LOCKED,ms);
        assert(a>=previous && a>=5 && a<=100);previous=a;
    }
    for(unsigned ms=0;ms<100000;ms++) {
        unsigned a=led_motion_level(LED_MOTION_ALL_LOCKED,ms);
        unsigned b=led_motion_level(LED_MOTION_MIXED,ms);
        assert(a>=5 && a<=100 && b>=65 && b<=100);
        assert(a==led_motion_level(LED_MOTION_ALL_LOCKED,ms+2400));
        assert(led_motion_level(LED_MOTION_NORMAL,ms)==100);
    }
    puts("PASS: encrypted-frame body format, legacy/malformed/expired/disconnected unknown; dual-peer matrix; breathing bounds/smoothness/period");
}
