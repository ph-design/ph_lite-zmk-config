#include <assert.h>
#include <stdio.h>
#include "studio-radio-link.h"
int main(void) {
    struct sr_link a,b;sr_reset(&a,0x12345678);sr_reset(&b,a.session);
    uint8_t packet[32],reply[32],data[12];
    for(int i=0;i<12;i++)data[i]=i;
    for(int round=0;round<600;round++) {
        sr_stage(&a,data,12);size_t len=sr_encode(&a,0,packet);
        assert(len==20 && sr_valid(packet,len) && sr_session(packet)==a.session);
        assert(sr_data_ready(&b,packet,len));b.rx++;
        // A lost ACK causes a replay of the fragment, which must not deliver twice.
        assert(!sr_data_ready(&b,packet,len));
        sr_encode(&b,0,reply);sr_ack(&a,reply);assert(!a.pending_len && a.tx==b.rx);
    }
    sr_stage(&a,data,3);sr_encode(&b,0,reply);reply[7]=(uint8_t)(a.tx+2);
    sr_ack(&a,reply);assert(a.pending_len==3);
    assert(!sr_valid(packet,7));assert(!sr_valid(packet,21));
    packet[1]=0x80;assert(!sr_valid(packet,20));
    sr_reset(&b,44);assert(b.rx==0 && b.tx==0 && !b.pending_len);
    puts("radio stream: fragmentation, loss, duplicate suppression, rollover and invalid headers PASS");
}
