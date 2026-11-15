/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "led-link.h"
#include "led-pattern.h"
static bool level;
static bool run(struct led_link_state *s, uint32_t now, uint8_t mask) {
    return led_link_update(s, now, mask, true, false, &level);
}
int main(void) {
    struct led_link_state s = {0};
    assert(!run(&s,0,0));
    assert(run(&s,10,1) && !level);
    assert(run(&s,110,1) && level);
    assert(run(&s,210,1) && !level);
    assert(run(&s,310,1) && level);
    assert(run(&s,410,1) && !level);
    assert(!run(&s,610,1));
    assert(!run(&s,10000,1)); /* stable packets never retrigger */
    assert(run(&s,11000,0) && !level);
    assert(run(&s,11100,0) && level);
    assert(run(&s,11400,0) && !level);
    assert(run(&s,11600,0) && level);
    assert(run(&s,11900,0) && !level);
    assert(!run(&s,12100,0));

    /* Peer 1 connects/disconnects while peer 0 remains online. */
    s=(struct led_link_state){0};run(&s,0,1);run(&s,600,1);
    assert(run(&s,1000,3) && s.connected);
    run(&s,1600,3);
    assert(run(&s,2000,1) && !s.connected);

    /* Both peers generate their own double-flash sequences. */
    s=(struct led_link_state){0};run(&s,0,3);
    assert(s.count==1 && s.active);
    assert(run(&s,600,3) && s.count==0);
    assert(!run(&s,1200,3));

    /* No blocking when a disconnect/reconnect arrives mid-animation. */
    s=(struct led_link_state){0};run(&s,0,1);run(&s,25,0);run(&s,50,1);
    assert(s.count==2);
    run(&s,600,1);assert(!s.connected);
    run(&s,1700,1);assert(s.connected);
    assert(!run(&s,2300,1));

    /* OFF and identify cancel notifications, tracking state without replay
     * of stale connection events after settings/identify are changed. */
    s=(struct led_link_state){0};run(&s,0,1);
    assert(!led_link_update(&s,25,0,false,false,&level));
    assert(!run(&s,50,0));
    assert(!led_link_update(&s,75,1,true,true,&level));
    assert(!run(&s,5100,1));

    s=(struct led_link_state){0};
    uint32_t start=UINT_MAX-200;
    run(&s,start,1);
    assert(run(&s,start+300u,1) && level);
    assert(!run(&s,start+600u,1));

    s=(struct led_link_state){0};
    for(unsigned i=0;i<10000;i++) {
        run(&s,i,(i&1)?1:0);assert(s.count<=LED_LINK_QUEUE);
    }
    /* Existing automatic/low-battery patterns retain their semantics. */
    assert(dongle_led_on(0,false,-1,false));
    assert(!dongle_led_on(1500,false,-1,false));
    assert(dongle_led_on(1500,true,80,false));
    assert(dongle_led_on(275,true,10,true));
    assert(!dongle_led_on(600,true,10,true));
    puts("PASS: link LED edges/timing, per-peer queue, OFF/identify, rollover, 10000-edge stress, baseline patterns");
}
