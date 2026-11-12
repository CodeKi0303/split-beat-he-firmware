#include "keyboard.h"
#include <string.h>
static void p16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
uint16_t c77_crc16(const uint8_t *p,size_t n){uint16_t c=0xffff;while(n--){c^=(uint16_t)*p++<<8;for(unsigned j=0;j<8;j++)c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));}return c;}
uint32_t c77_crc32(const uint8_t *p,size_t n){uint32_t c=0xffffffff;while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
void c77_defaults(c77_config *cfg){
    memset(cfg,0,sizeof *cfg);
    /* Provisional mirrored gaming layout; all positions are remappable. */
    const uint8_t usage[20]={0x39,0x14,0x1a,0x08,0x15,0xe1,0x2c, 0x2a,0x18,0x0c,0x12,0x13,0xe5,0x2c, 0x1e,0x21,0x28,0x27,0x24,0x28};
    for(unsigned i=0;i<C77_HALL;i++)cfg->keys[i]=(c77_key_config){.actuation=200,.rt_press=100,.rt_release=100,.top_deadzone=30,.noise=2,.flags=3};
    for(unsigned i=0;i<20;i++)cfg->bindings[i]=(c77_binding){.kind=1,.code=usage[i]};
    for(unsigned i=20;i<28;i++)cfg->bindings[i]=(c77_binding){.kind=(uint8_t)((i%4<2)?2:4),.code=(uint16_t)((i%4<2)?((i%2)?0xffff:1):((i%2)?0xea:0xe9))};
}
static bool consumer_valid(uint16_t code){return code==0xb5||code==0xb6||code==0xb7||code==0xcd||code==0xe2||code==0xe9||code==0xea;}
bool c77_config_valid(const c77_config *cfg){
    if(!cfg)return false;
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_key_config *k=&cfg->keys[i];
        int span=(int)k->pressed-(int)k->released;if(span<0)span=-span;
        if(k->flags>(HARDWARE_REVISION==86?15:7)||k->released>4095||k->pressed>4095||k->actuation>1000||k->actuation<=k->top_deadzone||k->bottom_deadzone>1000-k->actuation||k->curve_id>RT_CURVE_OWLAB_TI_REFERENCE||!k->rt_press||k->rt_press>1000||!k->rt_release||k->rt_release>1000||k->noise>100||((k->flags&4)&&span<128))return false;
    }
    for(unsigned i=0;i<C77_INPUTS;i++){
        const c77_binding *b=&cfg->bindings[i];
#if HARDWARE_REVISION == 86
        if(b->kind==C77_BINDING_KEY_CHORD){
            unsigned first=b->code&0xffu,second=b->code>>8;
            if(first<4u||second<=first||second>0xdfu)return false;
            if(b->third_key&&(b->third_key<=second||b->third_key>0xdfu))return false;
            continue;
        }
#endif
        if(b->kind>5||b->third_key)return false;
        if(b->kind==0&&(b->code||b->modifiers))return false;
        if(b->kind!=1&&b->modifiers)return false;
        if(b->kind==1&&b->code!=0&&(b->code<4||b->code>0xe7))return false;
        if((b->kind==2||b->kind==3)&&(b->code!=1&&b->code!=0xffff))return false;
        if(b->kind==4&&!consumer_valid(b->code))return false;
        if(b->kind==5&&(b->code<1||b->code>5))return false;
    }return true;
}
void c77_config_encode(const c77_config *cfg,uint8_t out[C77_CONFIG_BYTES]){
    memset(out,0,C77_CONFIG_BYTES);
    for(unsigned i=0;i<C77_HALL;i++){
        uint8_t *p=out+i*22;const c77_key_config *k=&cfg->keys[i];p[0]=(uint8_t)i;p[1]=k->flags;
        p16(p+2,k->released);p16(p+4,k->pressed);p16(p+6,k->actuation);p16(p+8,k->rt_press);p16(p+10,k->rt_release);p16(p+12,k->top_deadzone);p16(p+14,k->noise);p16(p+16,k->bottom_deadzone);p16(p+18,k->curve_id);
    }
    for(unsigned i=0;i<C77_INPUTS;i++){
        uint8_t *p=out+308+i*6;
        const c77_binding *b=&cfg->bindings[i];
        p[0]=(uint8_t)i;p[1]=b->kind;p16(p+2,b->code);p[4]=b->modifiers;p[5]=b->third_key;
    }
}
bool c77_config_decode(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]){
    c77_config tmp;memset(&tmp,0,sizeof tmp);
    for(unsigned i=0;i<C77_HALL;i++){
        const uint8_t *p=in+i*22;c77_key_config *k=&tmp.keys[i];if(p[0]!=i)return false;
        for(unsigned j=20;j<22;j++)if(p[j])return false;
        k->flags=p[1];k->released=g16(p+2);k->pressed=g16(p+4);k->actuation=g16(p+6);k->rt_press=g16(p+8);k->rt_release=g16(p+10);k->top_deadzone=g16(p+12);k->noise=g16(p+14);k->bottom_deadzone=g16(p+16);k->curve_id=g16(p+18);
    }
    for(unsigned i=0;i<C77_INPUTS;i++){
        const uint8_t *p=in+308+i*6;
        if(p[0]!=i)return false;
        tmp.bindings[i]=(c77_binding){.kind=p[1],.code=g16(p+2),.modifiers=p[4],.third_key=p[5]};
    }
    if(!c77_config_valid(&tmp))return false;
    *cfg=tmp;return true;
}
