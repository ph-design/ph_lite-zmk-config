/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "guard.h"
static struct mg_key output[1024];
static unsigned count;
static struct mg_guard g;
static int64_t now;
static void emit(struct mg_key k, void *ctx) {
    (void)ctx;
    assert(count < 1024);
    if (k.down) { assert(!mg_blocks(&g)); }
    output[count++] = k;
}
static void tick(int x, int y, int z) {
    now += 10; mg_sample(&g, now, x, y, z); mg_drain(&g,now);
}
static void flat(unsigned n) { while(n--) { tick(0,0,1000); } }
static void key(unsigned p, bool down) { mg_key(&g,(struct mg_key){p,down,now}); }
static void reset(void) { count=0; now=0; mg_init(&g,emit,NULL); }
static void ready(void) { reset(); flat(9); assert(g.state==MG_READY); }
int main(void) {
    reset();
    for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); key(1,true);key(1,false); }
    assert(count==0 && g.state==MG_CARRY);

    ready(); flat(200000);key(1,true);key(1,false);
    assert(count==2 && output[0].time==now);

    ready();key(2,true);tick(700,0,700);tick(700,0,700);
    assert(count==2 && !output[1].down && output[1].position==2);
    key(3,true);key(3,false);assert(count==2);
    key(4,true);flat(9);assert(g.state==MG_READY);
    key(4,true);key(4,false);assert(count==2);
    key(4,true);key(4,false);assert(count==4);

    ready();tick(900,0,100);tick(900,0,100);flat(2);
    assert(g.state==MG_SETTLING);
    key(5,true);flat(1);key(5,false);flat(8);
    assert(g.state==MG_READY && count==2);
    assert(output[0].down && !output[1].down && output[0].position==5);
    assert(output[1].time>output[0].time);

    /* A candidate never blocks desktop events or their release reports. */
    ready();tick(240,0,1000);tick(240,0,1000);assert(g.state==MG_VERIFY && !mg_blocks(&g));
    key(6,true);key(7,true);key(7,false);key(6,false);
    assert(count==4 && g.queued==0);
    flat(2);assert(g.state==MG_READY);

    ready();tick(240,0,1000);
    for(unsigned i=0;i<4;i++) { tick(240,0,1000); }
    assert(g.state==MG_CARRY);

    ready();tick(900,0,100);tick(900,0,100);flat(2);key(9,true);key(9,false);
    tick(900,0,100);tick(900,0,100);flat(10);assert(count==0);

    ready();now+=151;key(10,true);assert(count==0 && g.state==MG_CARRY);
    flat(1);assert(g.state!=MG_READY);flat(9);assert(g.state==MG_READY);
    key(10,false);assert(count==0);key(10,true);assert(count==1);

    ready();key(11,true);tick(900,0,100);tick(900,0,100);flat(2);
    for(unsigned i=0;i<MG_QUEUE+1;i++) { key(12,i%2==0); }
    assert(g.state==MG_CARRY && count==2 && !output[1].down);

    ready();tick(900,0,100);tick(900,0,100);flat(2);key(13,true);key(13,false);
    mg_enable(&g,false);assert(!mg_blocks(&g));
    assert(mg_drain(&g,now+4));assert(mg_drain(&g,now+8));
    assert(count==2 && !output[1].down);
    key(14,true);assert(count==3);
    mg_enable(&g,true);assert(g.state==MG_CARRY && !g.sent[14]);

    reset();
    for(unsigned i=0;i<10000;i++) {
        tick(i%2?900:-900,100,100);key(i%60,true);key(i%60,false);
    }
    assert(count==0);

    /* Rapid typing with strong 10ms vertical case vibrations is immediate. */
    ready();
    for(unsigned i=0;i<10000;i++) {
        count=0;
        tick(35,-25,1450);key(i%60,true);key(i%60,false);
        assert(!mg_blocks(&g) && g.queued==0 && count==2);
        assert(output[0].time==now && output[1].time==now);
        tick(0,0,1000);
        tick(-30,20,650);key((i+1)%60,true);key((i+1)%60,false);
        assert(!mg_blocks(&g) && g.queued==0 && count==4);
        tick(0,0,1000);
    }

    /* Continuous alternating vibration must not accumulate a carry streak. */
    ready();
    for(unsigned i=0;i<20000;i++) {
        count=0;tick(0,0,i%2 ? 650 : 1450);
        key(i%60,true);key(i%60,false);
        assert(!mg_blocks(&g) && g.queued==0 && count==2);
    }

    /* Releasing a held modifier during a candidate is never delayed. */
    ready();key(20,true);tick(240,0,1000);tick(240,0,1000);key(20,false);
    assert(count==2 && !output[1].down && output[1].time==now);

    /* Landing replay is bounded to one event per drain and preserves order
     * even when new typing arrives before the backlog empties. */
    ready();tick(900,0,100);tick(900,0,100);flat(2);
    key(21,true);key(21,false);key(22,true);key(22,false);
    while(g.state!=MG_READY) { now+=10;mg_sample(&g,now,0,0,1000); }
    assert(count==0 && g.queued==4);
    key(23,true);key(23,false);assert(count==0 && g.queued==6);
    for(unsigned i=0;i<6;i++) {
        now+=4;assert(mg_drain(&g,now));assert(count==i+1);
    }
    assert(output[0].position==21 && output[2].position==22 && output[4].position==23);

    /* Lock during a partially transmitted landing sequence releases only
     * the actually delivered press; the unsent tail never leaks. */
    ready();tick(900,0,100);tick(900,0,100);flat(2);key(24,true);key(24,false);
    while(g.state!=MG_READY) { now+=10;mg_sample(&g,now,0,0,1000); }
    mg_drain(&g,now);assert(count==1 && output[0].down);
    tick(900,0,100);tick(900,0,100);assert(count==2 && !output[1].down && g.queued==0);
    flat(12);assert(count==2);
    /* Regression: brief flat segments while continuously carried stay
     * locked, and their key events must never replay on a later landing. */
    reset();for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); }
    for(unsigned cycle=0;cycle<100;cycle++) {
        for(unsigned i=0;i<12;i++) {
            tick(0,0,1000);key(30,true);key(30,false);
            assert(mg_blocks(&g) && count==0);
        }
        tick(900,0,100);tick(900,0,100);assert(g.queued==0 && count==0);
    }

    /* Small per-sample deltas still represent motion if the whole window
     * drifts: old step-only detection incorrectly unlocked this trace. */
    reset();for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); }
    for(unsigned i=0;i<200;i++) {
        int phase=i%20;
        int x=phase<10 ? phase*20 : (20-phase)*20;
        tick(x,0,1000);key(31,true);key(31,false);
        assert(mg_blocks(&g) && count==0);
    }

    /* A real stable landing preserves its first short tap, but never
     * unlocks after the old 60ms window following prolonged transport. */
    reset();for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); }
    flat(2);assert(g.state==MG_SETTLING && g.settle_ms==180);
    key(32,true);flat(2);key(32,false);flat(5);
    assert(mg_blocks(&g) && count==0);
    flat(15);assert(g.state==MG_READY && count==2);
    assert(output[0].position==32 && output[0].down && !output[1].down);

    /* A press held in the bag cannot be converted to a new landing press. */
    reset();for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); }
    key(33,true);flat(25);assert(g.state==MG_READY && count==0);
    key(33,false);assert(count==0);key(33,true);key(33,false);assert(count==2);
    /* New config is validated as a whole, including cross-field constraints. */
    const struct mg_config defaults = MG_CONFIG_DEFAULTS;
    assert(mg_config_valid(&defaults));
    struct mg_config bad = defaults; bad.carried_settle_ms = 99; assert(!mg_config_valid(&bad));
    bad = defaults; bad.brief_settle_ms = 201; assert(!mg_config_valid(&bad));
    bad = defaults; bad.stable_span_mg = 101; assert(!mg_config_valid(&bad));
    bad = defaults; bad.departure_mg = 119; assert(!mg_config_valid(&bad));
    bad = defaults; bad.verify_ms = 61; assert(!mg_config_valid(&bad));
    bad = defaults; bad.carried_settle_ms = 100; bad.brief_settle_ms = 200; assert(!mg_config_valid(&bad));
    /* The long and brief confirmation values control actual unlocking. */
    reset(); g.config.carried_settle_ms = 400;
    for(unsigned i=0;i<100;i++) { tick(900,0,100);tick(900,0,100); }
    flat(30); assert(mg_blocks(&g)); flat(15); assert(!mg_blocks(&g));
    ready(); g.config.brief_settle_ms = 120; tick(900,0,100);tick(900,0,100);
    flat(9); assert(mg_blocks(&g)); flat(6); assert(!mg_blocks(&g));
    /* Threshold and confirmation duration affect ambiguous lifting only. */
    ready(); g.config.departure_mg = 350; tick(240,0,1000); assert(g.state == MG_READY);
    ready(); g.config.verify_ms = 60; for(unsigned i=0;i<4;i++) tick(240,0,1000);
    assert(!mg_blocks(&g)); for(unsigned i=0;i<4;i++) tick(240,0,1000); assert(mg_blocks(&g));
    /* A stricter stable-window span rejects movement below the old span. */
    reset(); g.config.stable_span_mg = 20; tick(900,0,100);tick(900,0,100); flat(2); tick(30,0,1000);
    assert(g.state == MG_CARRY);
    /* Hard bottom-outs including misleading one-frame sideways/inverted
     * vectors never release a held key or delay a subsequent key. */
    const int shocks[][3]={{700,0,700},{0,900,100},{0,0,-1000},{1600,0,300},{0,0,2000}};
    for(unsigned j=0;j<sizeof(shocks)/sizeof(shocks[0]);j++) {
        ready(); key(40,true); tick(shocks[j][0],shocks[j][1],shocks[j][2]);
        assert(!mg_blocks(&g) && g.sent[40]); key(41,true);key(41,false);
        flat(4);assert(!mg_blocks(&g) && g.sent[40] && count==3);key(40,false);
    }
    /* No immunity while holding a game key: a real lift still locks promptly. */
    ready();key(42,true);int64_t lift_started=now;tick(900,0,100);assert(!mg_blocks(&g));
    tick(900,0,100);assert(mg_blocks(&g) && now-lift_started==20 && !g.sent[42]);
    /* A stale sensor cannot reuse pose history to bypass lock. */
    ready();now+=151;tick(0,0,1000);assert(mg_blocks(&g));
    puts("PASS: policy scenarios + isolated impact/held-key lift/stale regressions + settings bounds/5 parameter effects; brief-flat carry/drifting-window regressions; 10000 bag cycles; 40000 vibration taps");
    return 0;
}
