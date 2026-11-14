/* SPDX-License-Identifier: MIT */
#include "input-merge.h"
#include <string.h>
void merge_keyboard(const struct input_merge *s,uint8_t out[15]) {
    for(int i=0;i<15;i++)out[i]=s->keyboard[0][i]|s->keyboard[1][i];
    out[1]=0;
}
void merge_consumer(const struct input_merge *s,uint8_t out[24]) {
    memset(out,0,24);unsigned used=0;
    for(int slot=0;slot<2;slot++)for(int i=0;i<12;i+=2) {
        const uint8_t *p=s->consumer[slot]+i;if(!p[0] && !p[1])continue;
        unsigned j;for(j=0;j<used;j++)if(out[2*j]==p[0] && out[2*j+1]==p[1])break;
        if(j==used){out[2*used]=p[0];out[2*used+1]=p[1];used++;}
    }
}
