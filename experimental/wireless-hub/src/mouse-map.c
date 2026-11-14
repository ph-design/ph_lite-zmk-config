/* SPDX-License-Identifier: MIT */
#include "mouse-map.h"
#include <errno.h>
#include <string.h>
struct global { uint32_t page,size,count; int32_t minimum; uint8_t id; };
static uint32_t val(const uint8_t *p,size_t n) {
    uint32_t v=0;for(size_t i=0;i<n;i++)v|=(uint32_t)p[i]<<(8*i);return v;
}
static struct mm_report *report(struct mouse_map *m,uint8_t id) {
    for(int i=0;i<m->report_count;i++)if(m->reports[i].id==id)return &m->reports[i];
    if(m->report_count==MM_REPORTS)return NULL;
    struct mm_report *r=&m->reports[m->report_count++];r->id=id;return r;
}
int mouse_map_parse(struct mouse_map *m,const uint8_t *p,size_t n) {
    if(!m || !p || !n || n>2048)return -EINVAL;
    memset(m,0,sizeof(*m));
    struct global g={0},stack[4];int sp=0,depth=0;
    bool inside=false,collections[16];
    uint32_t usages[32],min=0,max=0;unsigned count=0;bool range=false;
    for(size_t i=0;i<n;) {
        uint8_t prefix=p[i++];
        if(prefix==0xfe)return -ENOTSUP;
        unsigned size=prefix&3;if(size==3)size=4;
        if(size>n-i)return -EINVAL;
        uint32_t v=val(p+i,size);i+=size;
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1) {
            switch(tag) {
            case 0:g.page=v;break;
            case 1:g.minimum=(size && size<4 && (v&(1u<<(size*8-1))))?
                       (int32_t)(v|(~0u<<(size*8))):(int32_t)v;break;
            case 7:g.size=v;break;
            case 8:if(!v || v>255)return -EINVAL;g.id=v;break;
            case 9:g.count=v;break;
            case 10:if(sp==4)return -E2BIG;stack[sp++]=g;break;
            case 11:if(!sp)return -EINVAL;g=stack[--sp];break;
            default:break;
            }
        } else if(type==2) {
            // Four-byte local usages contain their own usage page.
            uint32_t usage=size==4?v:((g.page<<16)|(v&0xffff));
            if(tag==0){if(count==32)return -E2BIG;usages[count++]=usage;}
            else if(tag==1){min=usage;range=true;}
            else if(tag==2)max=usage;
        } else if(type==0) {
            if(tag==10) {
                if(depth==16)return -E2BIG;
                collections[depth++]=inside;
                if(v==1)inside=count && usages[0]==0x10002; // Mouse application only.
            } else if(tag==12) {
                if(!depth)return -EINVAL;
                inside=collections[--depth];
            } else if(tag==8) {
                if(g.size>32 || g.count>1024 || !g.size || !g.count)return -EINVAL;
                struct mm_report *r=report(m,g.id);if(!r)return -E2BIG;
                if(g.size*g.count>8192u-r->bits)return -E2BIG;
                if(inside && !(v&1) && (v&2)) {
                    for(uint32_t j=0;j<g.count;j++) {
                        uint32_t u=j<count?usages[j]:(range && min<=max && j<=max-min?min+j:0);
                        int kind=-1;unsigned button=0;
                        if((u>>16)==9 && (u&0xffff)>=1 && (u&0xffff)<=8) {
                            kind=MM_BUTTON;button=(u&0xffff)-1;
                        } else if(v&4) {
                            if(u==0x10030)kind=MM_X;
                            if(u==0x10031)kind=MM_Y;
                            if(u==0x10038)kind=MM_WHEEL;
                            if(u==0xc0238)kind=MM_PAN;
                        }
                        if(kind>=0) {
                            if(m->field_count==MM_FIELDS)return -E2BIG;
                            m->fields[m->field_count++]=(struct mm_field){
                                .offset=r->bits+j*g.size,.id=g.id,.width=g.size,
                                .kind=kind,.button=button,.sign=g.minimum<0};
                        }
                    }
                }
                r->bits+=g.size*g.count;
            }
            count=0;min=0;max=0;range=false;
        }
    }
    if(depth || sp || !m->field_count)return -ENOTSUP;
    return 0;
}
int mouse_map_decode(const struct mouse_map *m,uint8_t id,const uint8_t *p,
                     size_t n,struct mouse_event *out) {
    if(!m || !p || !out)return -EINVAL;
    memset(out,0,sizeof(*out));
    bool known=false;
    for(int i=0;i<m->report_count;i++)if(m->reports[i].id==id) {
        known=true;if(n<(m->reports[i].bits+7u)/8u)return -EMSGSIZE;
    }
    if(!known)return -ENOENT;
    for(int i=0;i<m->field_count;i++) {
        const struct mm_field *f=&m->fields[i];if(f->id!=id)continue;
        uint32_t v=0;
        for(unsigned b=0;b<f->width;b++)v|=((p[(f->offset+b)/8]>>((f->offset+b)%8))&1u)<<b;
        if(f->sign && f->width<32 && (v&(1u<<(f->width-1))))v|=~0u<<f->width;
        switch(f->kind) {
        case MM_BUTTON:out->button_mask|=1u<<f->button;if(v)out->buttons|=1u<<f->button;break;
        case MM_X:out->x=(int32_t)v;break;
        case MM_Y:out->y=(int32_t)v;break;
        case MM_WHEEL:out->wheel=(int32_t)v;break;
        case MM_PAN:out->pan=(int32_t)v;break;
        }
    }
    return 0;
}
