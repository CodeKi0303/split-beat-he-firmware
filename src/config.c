#include "keyboard.h"
#include <string.h>
static void p16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
uint16_t c77_crc16(const uint8_t *p,size_t n){uint16_t c=0xffff;while(n--){c^=(uint16_t)*p++<<8;for(unsigned j=0;j<8;j++)c=(uint16_t)((c<<1)^((c&0x8000)?0x1021:0));}return c;}
uint32_t c77_crc32(const uint8_t *p,size_t n){uint32_t c=0xffffffff;while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^((c&1)?0xedb88320u:0);}return ~c;}
void c77_defaults(c77_config *cfg){
    memset(cfg,0,sizeof *cfg);
    /* Mapping-only baseline: default-bindings.json. A valid saved profile
     * replaces these defaults at startup; calibration/RT defaults stay local. */
    static const c77_binding bindings[C77_INPUTS]={
        {.kind=1,.code=0x2b}, {.kind=1,.code=0x04},
        {.kind=1,.code=0x16}, {.kind=1,.code=0x07},
        {.kind=1,.code=0x09}, {.kind=1,.code=0xe1},
        {.kind=1,.code=0x2c}, {.kind=1,.code=0x28},
        {.kind=1,.code=0x33}, {.kind=1,.code=0x0f},
        {.kind=1,.code=0x0e}, {.kind=1,.code=0x0d},
        {.kind=1,.code=0xe5}, {.kind=1,.code=0,.modifiers=0x40},
        {.kind=1,.code=0x3a}, {.kind=1,.code=0x3e},
        {.kind=1,.code=0x29}, {.kind=1,.code=0x4f},
        {.kind=1,.code=0x50}, {.kind=1,.code=0x28},
        {.kind=1,.code=0x3a}, {.kind=1,.code=0x3b},
        {.kind=1,.code=0x4b}, {.kind=1,.code=0x4e},
        {.kind=2,.code=1}, {.kind=2,.code=0xffff},
        {.kind=1,.code=0x52}, {.kind=1,.code=0x51}
    };
    for(unsigned i=0;i<C77_HALL;i++)cfg->keys[i]=(c77_key_config){.actuation=2000,.rt_press=1000,.rt_release=1000,.top_deadzone=300,.noise=2,.flags=3};
    memcpy(cfg->bindings,bindings,sizeof bindings);
    rt_custom_curve_linear(cfg->curve);
}
void c77_curve_encode(const uint16_t curve[RT_CUSTOM_CURVE_POINTS],uint8_t out[C77_CURVE_BYTES]){
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)p16(out+2*i,curve[i]);
}
bool c77_curve_decode(uint16_t curve[RT_CUSTOM_CURVE_POINTS],const uint8_t in[C77_CURVE_BYTES]){
    uint16_t tmp[RT_CUSTOM_CURVE_POINTS];
    for(unsigned i=0;i<RT_CUSTOM_CURVE_POINTS;i++)tmp[i]=g16(in+2*i);
    if(!rt_custom_curve_valid(tmp))return false;
    memcpy(curve,tmp,sizeof tmp);return true;
}
uint32_t c77_config_crc(const c77_config *cfg){
    uint8_t blob[C77_CONFIG_BYTES+C77_CURVE_BYTES];
    c77_config_encode(cfg,blob);c77_curve_encode(cfg->curve,blob+C77_CONFIG_BYTES);
    return c77_crc32(blob,sizeof blob);
}
static bool consumer_valid(uint16_t code){return code==0xb5||code==0xb6||code==0xb7||code==0xcd||code==0xe2||code==0xe9||code==0xea;}
bool c77_config_valid(const c77_config *cfg){
    if(!cfg||!rt_custom_curve_valid(cfg->curve))return false;
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_key_config *k=&cfg->keys[i];
        int span=(int)k->pressed-(int)k->released;if(span<0)span=-span;
        if(k->fixed_release&&(k->fixed_release<k->top_deadzone||k->fixed_release>=k->actuation))return false;
        if(k->curve_id>(HARDWARE_REVISION==86?RT_CURVE_WISH_OWLAB_TI:RT_CURVE_CUSTOM))return false;
        if(k->flags>(HARDWARE_REVISION==86?15:7)||k->released>4095||k->pressed>4095||k->actuation>RT_TRAVEL_MAX||k->actuation<=k->top_deadzone||k->bottom_deadzone>RT_TRAVEL_MAX-k->actuation||!k->rt_press||k->rt_press>RT_TRAVEL_MAX||!k->rt_release||k->rt_release>RT_TRAVEL_MAX||k->noise>100||((k->flags&4)&&span<128))return false;
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
        if(b->kind==C77_BINDING_POINTER){
            const int16_t speed=(int16_t)b->code;
            if(b->modifiers>1||b->third_key>C77_POINTER_ACCEL_MAX||(i<20&&b->third_key)||
               !speed||speed>C77_POINTER_MAX||speed< -C77_POINTER_MAX)return false;
            continue;
        }
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
        p16(p+2,k->released);p16(p+4,k->pressed);p16(p+6,k->actuation);p16(p+8,k->rt_press);p16(p+10,k->rt_release);p16(p+12,k->top_deadzone);p16(p+14,k->noise);p16(p+16,k->bottom_deadzone);p16(p+18,k->curve_id);p16(p+20,k->fixed_release);
    }
    for(unsigned i=0;i<C77_INPUTS;i++){
        uint8_t *p=out+308+i*6;
        const c77_binding *b=&cfg->bindings[i];
        p[0]=(uint8_t)i;p[1]=b->kind;p16(p+2,b->code);p[4]=b->modifiers;p[5]=b->third_key;
    }
}
bool c77_config_decode(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]){
    /* The custom curve is not in this encoding: start from the identity table.
     * Callers that keep a curve (SET_KEY, journal format 79) restore it. */
    c77_config tmp;memset(&tmp,0,sizeof tmp);rt_custom_curve_linear(tmp.curve);
    for(unsigned i=0;i<C77_HALL;i++){
        const uint8_t *p=in+i*22;c77_key_config *k=&tmp.keys[i];if(p[0]!=i)return false;
        k->fixed_release=g16(p+20);
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
/* Journal format77 (firmware <=0.3.42) stores travel thresholds in 0..1000.
 * Multiplying every distance by10 keeps each threshold at the same fraction of
 * the stroke and keeps every ordering constraint valid. Raw ADC endpoints,
 * noise (ADC codes), curve, flags and bindings do not change. */
bool c77_config_decode_legacy(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]){
    c77_config tmp;
    if(!c77_config_decode(&tmp,in))return false;
    for(unsigned i=0;i<C77_HALL;i++){
        c77_key_config *k=&tmp.keys[i];
        if(k->fixed_release||k->actuation>RT_LEGACY_TRAVEL_MAX||k->rt_press>RT_LEGACY_TRAVEL_MAX||k->rt_release>RT_LEGACY_TRAVEL_MAX||
           k->bottom_deadzone>RT_LEGACY_TRAVEL_MAX-k->actuation)return false;
        k->actuation=(uint16_t)(k->actuation*RT_LEGACY_TRAVEL_SCALE);
        k->rt_press=(uint16_t)(k->rt_press*RT_LEGACY_TRAVEL_SCALE);
        k->rt_release=(uint16_t)(k->rt_release*RT_LEGACY_TRAVEL_SCALE);
        k->top_deadzone=(uint16_t)(k->top_deadzone*RT_LEGACY_TRAVEL_SCALE);
        k->bottom_deadzone=(uint16_t)(k->bottom_deadzone*RT_LEGACY_TRAVEL_SCALE);
    }
    if(!c77_config_valid(&tmp))return false;
    *cfg=tmp;return true;
}
void c77_profile_capture(c77_profile *p,const c77_config *cfg){
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_key_config *k=&cfg->keys[i];
        p->keys[i]=(c77_profile_key){.actuation=k->actuation,.rt_press=k->rt_press,.rt_release=k->rt_release,
            .top_deadzone=k->top_deadzone,.bottom_deadzone=k->bottom_deadzone,.fixed_release=k->fixed_release,.flags=(uint8_t)(k->flags&C77_PROFILE_KEY_FLAGS)};
    }
    memcpy(p->bindings,cfg->bindings,sizeof p->bindings);
}
bool c77_profile_apply(c77_config *cfg,const c77_profile *p){
    c77_config tmp=*cfg;
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_profile_key *s=&p->keys[i];c77_key_config *k=&tmp.keys[i];
        if(s->flags&~C77_PROFILE_KEY_FLAGS)return false;
        k->actuation=s->actuation;k->rt_press=s->rt_press;k->rt_release=s->rt_release;
        k->fixed_release=s->fixed_release;
        k->top_deadzone=s->top_deadzone;k->bottom_deadzone=s->bottom_deadzone;
        k->flags=(uint8_t)((k->flags&~C77_PROFILE_KEY_FLAGS)|s->flags);
    }
    memcpy(tmp.bindings,p->bindings,sizeof tmp.bindings);
    if(!c77_config_valid(&tmp))return false;
    *cfg=tmp;return true;
}
void c77_profiles_init(c77_profile_bank *bank,const c77_config *cfg){
    memset(bank,0,sizeof *bank);bank->used=1;c77_profile_capture(&bank->slots[0],cfg);
}
bool c77_profile_name_valid(const uint8_t name[C77_PROFILE_NAME_BYTES]){
    bool ended=false;
    for(unsigned i=0;i<C77_PROFILE_NAME_BYTES;i++){
        if(!name[i]){ended=true;continue;}
        if(ended||name[i]<0x20u||name[i]==0x7fu)return false;
    }
    return true;
}
/* Field by field: struct padding is not part of a profile. */
static bool profile_empty(const c77_profile *p){
    for(unsigned i=0;i<C77_HALL;i++){
        const c77_profile_key *k=&p->keys[i];
        if(k->actuation||k->rt_press||k->rt_release||k->top_deadzone||k->bottom_deadzone||k->fixed_release||k->flags)return false;
    }
    for(unsigned i=0;i<C77_INPUTS;i++){
        const c77_binding *b=&p->bindings[i];
        if(b->code||b->kind||b->modifiers||b->third_key)return false;
    }
    for(unsigned i=0;i<C77_PROFILE_NAME_BYTES;i++)if(p->name[i])return false;
    return true;
}
bool c77_profiles_valid(const c77_profile_bank *bank,const c77_config *cfg){
    if(!bank||!cfg||bank->active>=C77_PROFILES||bank->used>0x0fu||!(bank->used&(1u<<bank->active)))return false;
    for(unsigned s=0;s<C77_PROFILES;s++){
        const c77_profile *p=&bank->slots[s];
        if(!(bank->used&(1u<<s))){if(!profile_empty(p))return false;continue;}
        c77_config probe=*cfg;
        if(!c77_profile_name_valid(p->name)||!c77_profile_apply(&probe,p))return false;
    }
    return true;
}
void c77_profiles_encode(const c77_profile_bank *bank,uint8_t out[C77_PROFILE_BANK_BYTES]){
    memset(out,0,C77_PROFILE_BANK_BYTES);out[0]=bank->active;out[1]=bank->used;
    for(unsigned s=0;s<C77_PROFILES;s++){
        uint8_t *p=out+2+s*C77_PROFILE_BYTES;const c77_profile *src=&bank->slots[s];
        for(unsigned i=0;i<C77_HALL;i++){
            uint8_t *k=p+i*11;const c77_profile_key *v=&src->keys[i];
            k[0]=v->flags;p16(k+1,v->actuation);p16(k+3,v->rt_press);p16(k+5,v->rt_release);p16(k+7,v->top_deadzone);p16(k+9,v->bottom_deadzone);
            p16(out+C77_V80_BANK_BYTES+2*(s*C77_HALL+i),v->fixed_release);
        }
        for(unsigned i=0;i<C77_INPUTS;i++){
            uint8_t *b=p+C77_HALL*11+i*6;const c77_binding *v=&src->bindings[i];
            /* Unused slots stay all zero, so their binding index bytes are 0. */
            b[0]=(uint8_t)((bank->used&(1u<<s))?i:0u);b[1]=v->kind;p16(b+2,v->code);b[4]=v->modifiers;b[5]=v->third_key;
        }
        memcpy(p+C77_HALL*11+C77_INPUTS*6,src->name,C77_PROFILE_NAME_BYTES);
    }
}
bool c77_profiles_decode(c77_profile_bank *bank,const uint8_t *in,size_t length,const c77_config *cfg){
    if(length!=C77_V80_BANK_BYTES&&length!=C77_PROFILE_BANK_BYTES)return false;
    c77_profile_bank tmp;memset(&tmp,0,sizeof tmp);tmp.active=in[0];tmp.used=in[1];
    if(tmp.used>0x0fu)return false;
    for(unsigned s=0;s<C77_PROFILES;s++){
        const uint8_t *p=in+2+s*C77_PROFILE_BYTES;c77_profile *dst=&tmp.slots[s];const bool used=(tmp.used&(1u<<s))!=0;
        for(unsigned i=0;i<C77_HALL;i++){
            const uint8_t *k=p+i*11;
            dst->keys[i]=(c77_profile_key){.flags=k[0],.actuation=g16(k+1),.rt_press=g16(k+3),.rt_release=g16(k+5),.top_deadzone=g16(k+7),.bottom_deadzone=g16(k+9)};
            if(length==C77_PROFILE_BANK_BYTES)
                dst->keys[i].fixed_release=g16(in+C77_V80_BANK_BYTES+2*(s*C77_HALL+i));
        }
        for(unsigned i=0;i<C77_INPUTS;i++){
            const uint8_t *b=p+C77_HALL*11+i*6;
            if(b[0]!=(used?i:0u))return false;
            dst->bindings[i]=(c77_binding){.kind=b[1],.code=g16(b+2),.modifiers=b[4],.third_key=b[5]};
        }
        memcpy(dst->name,p+C77_HALL*11+C77_INPUTS*6,C77_PROFILE_NAME_BYTES);
    }
    if(!c77_profiles_valid(&tmp,cfg))return false;
    *bank=tmp;return true;
}
