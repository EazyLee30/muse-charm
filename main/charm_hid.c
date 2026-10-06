// Bounded HID short-item parser: Report IDs, padding, button usages and hat.
#include "charm_hid.h"
#include <string.h>
typedef struct {uint32_t page,size,count,id;int32_t min;} global_t;
bool charm_hid_parse(charm_hid_map_t *map,const uint8_t *d,size_t len) {
    memset(map,0,sizeof(*map));global_t g={0},stack[4];unsigned depth=0;
    uint16_t bits[256]={0};uint32_t usages[32],nusage=0,umin=0,umax=0;
    for(size_t pos=0;pos<len;) {
        unsigned tag=d[pos++];if(tag==0xfe) return false;
        unsigned size=tag&3; if(size==3) size=4;
        if(pos+size>len) return false;
        uint32_t value=0;for(unsigned i=0;i<size;i++) value|=(uint32_t)d[pos++]<<(8*i);
        unsigned type=(tag>>2)&3,item=tag>>4;
        if(type==1) switch(item) {
            case 0:g.page=value;break;
            case 1:g.min=size && size<4 && (value&(1u<<(size*8-1)))?(int32_t)(value|(~0u<<(size*8))):(int32_t)value;break;
            case 7:g.size=value;break;
            case 8:if(!value || value>255) return false;g.id=value;break;
            case 9:g.count=value;break;
            case 10:if(depth==4) return false;stack[depth++]=g;break;
            case 11:if(!depth) return false;g=stack[--depth];break;
        }
        if(type==2) {
            if(item==0 && nusage<32) usages[nusage++]=value;
            if(item==1) umin=value;
            if(item==2) umax=value;
        }
        if(type==0) {
            if(item==8) {
                if(!g.size || g.size>32 || g.count>512 || g.size*g.count>512 || bits[g.id]+g.size*g.count>512) return false;
                if(!(value&1) && (value&2)) for(unsigned i=0;i<g.count;i++) {
                    uint32_t usage=i<nusage?usages[i]:umin && umin+i<=umax?umin+i:0;
                    bool button=g.page==9 && usage>=1 && usage<=16;
                    bool hat=g.page==1 && usage==0x39;
                    if(button || hat) {
                        if(map->count==32) return false;
                        map->fields[map->count++]=(charm_hid_field_t){.bit=bits[g.id]+i*g.size,
                            .size=g.size,.id=g.id,.button=button?usage:0,.hat=hat,.minimum=g.min};
                    }
                }
                bits[g.id]+=g.size*g.count;
            }
            nusage=umin=umax=0;
        }
    }
    return map->count>0;
}
bool charm_hid_input(const charm_hid_map_t *map,uint8_t id,const uint8_t *d,size_t len,uint16_t *buttons,int *hat) {
    *buttons=0;*hat=-1;bool seen=false;
    for(unsigned i=0;i<map->count;i++) {
        const charm_hid_field_t *f=&map->fields[i];if(f->id!=id) continue;
        if(f->bit+f->size>len*8) return false;
        uint32_t value=0;for(unsigned b=0;b<f->size;b++) value|=((d[(f->bit+b)/8]>>((f->bit+b)%8))&1u)<<b;
        if(f->button && value) *buttons|=1u<<(f->button-1);
        if(f->hat) {int v=(int)value-f->minimum;*hat=v>=0 && v<8?v:-1;}
        seen=true;
    }
    return seen;
}
