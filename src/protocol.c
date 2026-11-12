#include "keyboard.h"
#include "hardware_profile.h"
#include "scan_profile.h"
#include "adc_trace.h"
#include "cpu_profile.h"
#include "adc_irq_probe.h"
#include "fw_update.h"
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
    const uint8_t sizes[11]={255,0,1,1,22,2,1,0,0,1,6};
    if(q[3]==0||q[3]>10){status=C77_BAD_COMMAND;goto done;}
    if(q[7]!=sizes[q[3]]){status=C77_BAD_PACKET;goto done;}
    if(e->save_requested&&q[3]!=1&&q[3]!=2&&q[3]!=3&&q[3]!=9&&!(q[3]==6&&in[0]==0)){status=C77_BUSY;goto done;}
    if(e->cal_state&&(q[3]==4||q[3]==7||q[3]==8||q[3]==10)){status=C77_BUSY;goto done;}
    switch(q[3]){
    case 1:
        out[0]=(uint8_t)((FW_UPDATE_VERSION>>24)&255U);out[1]=(uint8_t)((FW_UPDATE_VERSION>>16)&255U);out[2]=(uint8_t)(FW_UPDATE_VERSION&255U);
        out[3]=0;out[4]=14;out[5]=6;out[6]=4;out[7]=28;p32(out+8,SCAN_TARGET_HZ);p32(out+12,speed==2?125:1000);
        uint32_t capabilities=(HARDWARE_REVISION==86?0x3ffu:0x33fu)|C77_CAP_SAVE_OUTPUT;
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
        p32(out+16,capabilities);p32(out+20,e->generation);c77_config_encode(&e->config,blob);p32(out+24,c77_crc32(blob,sizeof blob));p16(out+28,flash);out[30]=HARDWARE_REVISION;out[31]=1;out[32]=speed;out[33]=e->stored_valid;length=34;break;
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
