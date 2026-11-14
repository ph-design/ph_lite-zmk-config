#include "mouse-map.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
int main(void) {
    const uint8_t standard[]={0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,
        0x05,9,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x81,2,
        0x75,3,0x95,1,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,
        0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,0xc0,0xc0};
    struct mouse_map m;struct mouse_event e;
    assert(!mouse_map_parse(&m,standard,sizeof(standard)));
    uint8_t data[]={0x11,0xfe,0x7f,0xff};
    assert(!mouse_map_decode(&m,0,data,sizeof(data),&e));
    assert(e.buttons==0x11 && e.button_mask==31 && e.x==-2 && e.y==127 && e.wheel==-1);
    assert(mouse_map_decode(&m,0,data,3,&e)==-EMSGSIZE);
    assert(mouse_map_decode(&m,7,data,4,&e)==-ENOENT);
    const uint8_t packed[]={0x05,1,0x09,2,0xa1,1,0x85,7,0x09,0x30,0x09,0x31,
        0x16,0,0xf8,0x26,0xff,7,0x75,12,0x95,2,0x81,6,0xc0};
    assert(!mouse_map_parse(&m,packed,sizeof(packed)));
    uint8_t p[]={0xff,0x0f,0x80};
    assert(!mouse_map_decode(&m,7,p,3,&e));assert(e.x==-1 && e.y==-2048);
    const uint8_t keyboard[]={0x05,1,0x09,6,0xa1,1,0x09,0x30,0x75,8,0x95,1,0x81,6,0xc0};
    assert(mouse_map_parse(&m,keyboard,sizeof(keyboard))==-ENOTSUP);
    const uint8_t malformed[]={0x75};assert(mouse_map_parse(&m,malformed,1)==-EINVAL);
    // Every truncation is bounded and can never produce a usable partial map.
    for(size_t i=0;i<sizeof(standard);i++)assert(mouse_map_parse(&m,standard,i)<0);
    puts("BLE mouse map: standard buttons/wheel, signed 12-bit, report IDs and malformed input PASS");
}
