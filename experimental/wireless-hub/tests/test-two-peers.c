#include "peer-crypto.h"
#include "input-merge.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    struct peer_crypto tx[2],rx[2];memset(tx,0,sizeof(tx));memset(rx,0,sizeof(rx));
    for(int s=0;s<2;s++) {
        memset(tx[s].key,s+1,16);memcpy(rx[s].key,tx[s].key,16);tx[s].tx_session=7;
    }
    uint8_t p[2][32]={{1,2,3},{1,4,5}},original[32];
    assert(peer_encrypt(&tx[0],1,p[0],3,32)==15);
    assert(peer_encrypt(&tx[1],1,p[1],3,32)==15);
    memcpy(original,p[0],15);
    assert(peer_decrypt(&rx[1],1,p[0],15)==-EBADMSG);
    assert(!rx[1].rx_initialized);
    assert(peer_decrypt(&rx[0],0,p[0],15)==-EBADMSG);
    assert(peer_decrypt(&rx[0],1,p[0],15)==3 && p[0][1]==2);
    assert(peer_decrypt(&rx[1],1,p[1],15)==3 && p[1][1]==4);
    assert(peer_decrypt(&rx[0],1,original,15)==-EACCES);
    uint8_t late[32]={1},next[32]={1};
    assert(peer_encrypt(&tx[0],1,late,1,32)==13);
    assert(peer_encrypt(&tx[0],1,next,1,32)==13);
    assert(peer_decrypt(&rx[0],1,next,13)==1);
    assert(peer_decrypt(&rx[0],1,late,13)==1);
    tx[0].tx_counter=UINT32_MAX;assert(peer_encrypt(&tx[0],0,next,1,32)==-ERANGE);
    struct input_merge m={0};uint8_t kb[15],cc[24];
    m.keyboard[0][0]=1;m.keyboard[1][0]=2;
    m.keyboard[0][2]=0x30;m.keyboard[1][2]=0x60;
    merge_keyboard(&m,kb);assert(kb[0]==3 && kb[2]==0x70);
    memset(m.keyboard[0],0,15);merge_keyboard(&m,kb);assert(kb[0]==2 && kb[2]==0x60);
    m.consumer[0][0]=0xe9;m.consumer[1][0]=0xe9;m.consumer[1][2]=0xea;
    merge_consumer(&m,cc);assert(cc[0]==0xe9 && cc[2]==0xea && !cc[4]);
    for(int s=0;s<2;s++)for(int i=0;i<6;i++){m.consumer[s][2*i]=1+s*6+i;m.consumer[s][2*i+1]=0;}
    merge_consumer(&m,cc);for(int i=0;i<12;i++)assert(cc[2*i]==i+1);
    puts("Two peers: key/direction/replay isolation, reorder, nonce exhaustion, overlapping keys, consumer union PASS");
}
