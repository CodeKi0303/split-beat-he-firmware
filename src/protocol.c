#include "keyboard.h"
#include "hardware_profile.h"
#include "scan_profile.h"
#include "adc_trace.h"
#include "cpu_profile.h"
#include "adc_irq_probe.h"
#include "fw_update.h"
#include "telemetry.h"
#if SCAN_ADC_PHASE_PAIR
#include "adc_async.h"
#endif
#include <string.h>
extern void c77_reconfigure(c77_engine *e);
static uint16_t g16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static void p16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void p32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
bool c77_command(c77_engine *e,const uint8_t q[64],uint8_t r[64],uint32_t now,uint8_t speed,uint16_t flash){
    if(q[0]!='C'||q[1]!='7')return false;
    memset(r,0,64);r[0]='C';r[1]='7';r[2]=1;r[3]=q[3]|0x80;r[4]=q[4];r[5]=q[5];
    uint8_t status=0,length=0;uint8_t *out=r+8;const uint8_t *in=q+8;uint8_t blob[C77_CONFIG_BYTES];
    if(q[2]!=1||q[3]&0x80||!g16(q+4)||q[6]||q[7]>54||c77_crc16(q,62)!=g16(q+62)){status=C77_BAD_PACKET;goto done;}
    for(unsigned i=8+q[7];i<62;i++)if(q[i]){status=C77_BAD_PACKET;goto done;}
    if(q[3]==C77_OP_CUSTOM_CURVE){
        /* Empty request reads; 30 bytes replace the shared custom curve in RAM.
         * Like SET_KEY, a write releases keys, turns output off, marks dirty. */
        if(q[7]!=0&&q[7]!=C77_CURVE_BYTES){status=C77_BAD_PACKET;goto done;}
        if(q[7]){
            if(e->save_requested||e->cal_state){status=C77_BUSY;goto done;}
            uint16_t curve[RT_CUSTOM_CURVE_POINTS];
            if(!c77_curve_decode(curve,in)){status=C77_BAD_CONFIG;goto done;}
            memcpy(e->config.curve,curve,sizeof curve);c77_reconfigure(e);
        }
        c77_curve_encode(e->config.curve,out);length=C77_CURVE_BYTES;goto done;
    }
    if(q[3]==C77_OP_PROFILE){
        /* [0] state; [1,i] name; [2,i] select; [3,i,source] create from a copy;
         * [4,i] delete; [5,i,name16] rename. Every reply but name returns
         * [count,active,used]. Mutations mark the RAM configuration dirty;
         * select also releases keys and turns output off like SET_KEY. */
        static const uint8_t lengths[6]={1,2,2,3,2,2+C77_PROFILE_NAME_BYTES};
        c77_profile_bank *b=&e->profiles;const unsigned sub=q[7]?in[0]:255u,index=q[7]>1?in[1]:0u;
        if(!q[7]||sub>5u||q[7]!=lengths[sub]){status=C77_BAD_PACKET;goto done;}
        if(sub>=2u&&(e->save_requested||e->cal_state)){status=C77_BUSY;goto done;}
        if(sub>=1u&&index>=C77_PROFILES){status=C77_BAD_INDEX;goto done;}
        const unsigned bit=1u<<index;
        if(sub==1u){out[0]=(uint8_t)index;memcpy(out+1,b->slots[index].name,C77_PROFILE_NAME_BYTES);length=1+C77_PROFILE_NAME_BYTES;goto done;}
        if(sub==2u){
            if(!(b->used&bit)){status=C77_BAD_INDEX;goto done;}
            if(index!=b->active){
                c77_config candidate=e->config;
                if(!c77_profile_apply(&candidate,&b->slots[index])){status=C77_BAD_CONFIG;goto done;}
                c77_profile_capture(&b->slots[b->active],&e->config);
                b->active=(uint8_t)index;e->config=candidate;c77_reconfigure(e);
            }
        }else if(sub==3u){
            const unsigned source=in[2];
            if((b->used&bit)||source>=C77_PROFILES||!(b->used&(1u<<source))){status=C77_BAD_INDEX;goto done;}
            c77_profile copy=b->slots[source];
            if(source==b->active)c77_profile_capture(&copy,&e->config);
            memset(copy.name,0,sizeof copy.name);
            b->slots[index]=copy;b->used|=(uint8_t)bit;e->dirty=1;
        }else if(sub==4u){
            if(!(b->used&bit)||index==b->active){status=C77_BAD_INDEX;goto done;}
            memset(&b->slots[index],0,sizeof b->slots[index]);b->used&=(uint8_t)~bit;e->dirty=1;
        }else if(sub==5u){
            if(!(b->used&bit)){status=C77_BAD_INDEX;goto done;}
            if(!c77_profile_name_valid(in+2)){status=C77_BAD_CONFIG;goto done;}
            memcpy(b->slots[index].name,in+2,C77_PROFILE_NAME_BYTES);e->dirty=1;
        }
        out[0]=C77_PROFILES;out[1]=b->active;out[2]=b->used;length=3;goto done;
    }
    const uint8_t sizes[11]={255,0,1,1,22,2,1,0,0,1,6};
    if(q[3]==0||q[3]>10){status=C77_BAD_COMMAND;goto done;}
    if(q[7]!=sizes[q[3]]){status=C77_BAD_PACKET;goto done;}
    if(e->save_requested&&q[3]!=1&&q[3]!=2&&q[3]!=3&&q[3]!=9&&!(q[3]==6&&in[0]==0)){status=C77_BUSY;goto done;}
    if(e->cal_state&&(q[3]==4||q[3]==7||q[3]==8||q[3]==10)){status=C77_BUSY;goto done;}
    switch(q[3]){
    case 1:
        out[0]=(uint8_t)((FW_UPDATE_VERSION>>24)&255U);out[1]=(uint8_t)((FW_UPDATE_VERSION>>16)&255U);out[2]=(uint8_t)(FW_UPDATE_VERSION&255U);
        out[3]=0;out[4]=14;out[5]=6;out[6]=4;out[7]=28;p32(out+8,SCAN_TARGET_HZ);p32(out+12,speed==2?125:1000);
        uint32_t capabilities=(HARDWARE_REVISION==86?0x3ffu:0x33fu)|C77_CAP_SAVE_OUTPUT|C77_CAP_FINE_TRAVEL|C77_CAP_CUSTOM_CURVE|C77_CAP_POINTER|C77_CAP_POINTER_ACCEL|C77_CAP_PROFILES;
#if SCAN_ADC_PHASE_PAIR
        p32(out+8,adc_scan_rate());
        capabilities|=C77_CAP_ADC_TUNING|C77_CAP_ADC_PAIR|C77_CAP_ADC_PAIR_READ;
#endif
#if HARDWARE_REVISION == 86
        capabilities|=C77_CAP_KEY_CHORD;
        capabilities|=C77_CAP_RETRIGGER;
#endif
#if SCAN_ADC_CAPTURE_DIAG
        capabilities|=ADC_TRACE_CAPABILITY;
#endif
#if defined(C86_CPU_PROFILE)
        capabilities|=CPU_PROFILE_CAPABILITY;
#endif
#if defined(C86_ADC_IRQ_PROBE)
        capabilities|=ADC_IRQ_PROBE_CAPABILITY;
#endif
#if C77_TELEMETRY
        capabilities|=TELEMETRY_CAPABILITY;
#endif
        p32(out+16,capabilities);p32(out+20,e->generation);p32(out+24,c77_config_crc(&e->config));p16(out+28,flash);out[30]=HARDWARE_REVISION;out[31]=1;out[32]=speed;out[33]=e->stored_valid;length=34;break;
    case 2:{
        unsigned side=in[0];if(side>1){status=C77_BAD_INDEX;break;}
        const c77_sample_t *s=&e->sample[side];p32(out,e->uptime_ms);p32(out+4,s->sequence);p32(out+8,e->errors+s->errors+e->pulse_drops);p32(out+12,e->seen[side]?now-s->timestamp_us:0xffffffffu);
        for(unsigned i=0;i<7;i++){p16(out+16+i*2,s->raw[i]);p16(out+30+i*2,e->travel[side*7+i]);}
        out[44]=s->digital;out[45]=(uint8_t)(e->output|(c77_source_fresh(e,side,now)?2:0)|(e->dirty?4:0)|(e->stored_valid?8:0));out[46]=e->cal_key;out[47]=e->cal_state;p32(out+48,e->pressed_mask);length=54;break;}
    case 3:case 9:{
        unsigned maximum=q[3]==3?14:28;if(in[0]>=maximum){status=C77_BAD_INDEX;break;}
        c77_config_encode(&e->config,blob);length=q[3]==3?22:6;memcpy(out,blob+(q[3]==3?in[0]*22:308+in[0]*6),length);break;}
    case 4:case 10:{
        unsigned maximum=q[3]==4?14:28;if(in[0]>=maximum){status=C77_BAD_INDEX;break;}
        c77_config_encode(&e->config,blob);length=q[3]==4?22:6;unsigned offset=q[3]==4?in[0]*22:308+in[0]*6;memcpy(blob+offset,in,length);
        c77_config candidate;if(!c77_config_decode(&candidate,blob)){status=C77_BAD_CONFIG;length=0;break;}
        memcpy(candidate.curve,e->config.curve,sizeof candidate.curve);
        e->config=candidate;c77_reconfigure(e);memcpy(out,blob+offset,length);break;}
    case 5:{
        unsigned key=in[0],action=in[1];if(key>=14){status=C77_BAD_INDEX;break;}if(action>3){status=C77_BAD_CONFIG;break;}
        if(action==3){c77_disable(e);e->cal_key=255;e->cal_state=0;}
        else if(action==0){if(e->cal_state){status=C77_BUSY;break;}if(!c77_source_fresh(e,key/7,now)){status=C77_NOT_READY;break;}c77_disable(e);e->cal_key=(uint8_t)key;e->cal_state=1;e->cal_count=0;e->cal_rest=e->cal_pressed=0;e->cal_started=now;}
        else if(action==1){if(e->cal_key!=key||e->cal_state!=2){status=C77_CAL_INVALID;break;}if(!c77_source_fresh(e,key/7,now)){status=C77_NOT_READY;break;}e->cal_state=3;e->cal_count=0;e->cal_started=now;}
        else {
            int span=(int)e->cal_pressed-e->cal_rest;if(span<0)span=-span;
            if(e->cal_key!=key||e->cal_state!=4||span<128){status=C77_CAL_INVALID;break;}
            e->config.keys[key].released=e->cal_rest;e->config.keys[key].pressed=e->cal_pressed;e->config.keys[key].flags|=4;e->cal_state=0;e->cal_key=255;c77_reconfigure(e);
        }
        e->restore_output_pending=false;
        out[0]=e->cal_key;out[1]=e->cal_state;p16(out+2,e->cal_rest);p16(out+4,e->cal_pressed);p16(out+6,e->cal_count);length=8;break;}
    case 6:
        if(in[0]>1){status=C77_BAD_CONFIG;break;}
        if(!in[0]){
            c77_disable(e);
            e->restore_output_pending=false;
            if(e->save_requested)e->save_output=false;
        }else if(!speed||!c77_enable_output(e,now)){status=C77_NOT_READY;break;}
        out[0]=e->output;length=1;break;
    case 7:
        if(flash!=1024||!c77_config_valid(&e->config)){status=C77_STORAGE_ERROR;break;}
        e->save_output=e->output!=0;
        e->restore_output_pending=false;
        c77_disable(e);
        e->save_requested=1;p32(out,e->generation==0xffffffffu?1:e->generation+1);length=4;break;
    case 8:c77_defaults(&e->config);c77_reconfigure(e);break;
    default:status=C77_BAD_COMMAND;break;
    }
done:
    r[6]=status;r[7]=status?0:length;if(status)memset(r+8,0,54);p16(r+62,c77_crc16(r,62));return true;
}
