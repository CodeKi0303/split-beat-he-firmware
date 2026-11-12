#include "keyboard.h"
#include "hardware_profile.h"
#include "fw_update.h"
#include "hall_filter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static c77_engine e;
static uint32_t now=100;
static uint16_t seq=1;
static void packet(uint8_t q[64],uint8_t op,const uint8_t *p,unsigned len){memset(q,0,64);q[0]='C';q[1]='7';q[2]=1;q[3]=op;q[4]=(uint8_t)seq;q[5]=(uint8_t)(seq++>>8);q[7]=(uint8_t)len;if(len)memcpy(q+8,p,len);uint16_t crc=c77_crc16(q,62);q[62]=(uint8_t)crc;q[63]=(uint8_t)(crc>>8);}
static unsigned command(uint8_t op,const uint8_t *p,unsigned len,uint8_t r[64]){uint8_t q[64];packet(q,op,p,len);assert(c77_command(&e,q,r,now,2,1024));assert(c77_crc16(r,62)==(uint16_t)(r[62]|((uint16_t)r[63]<<8)));return r[6];}
static void fresh(uint16_t raw){for(unsigned side=0;side<2;side++){c77_sample_t s={0};s.encoders[0]=e.encoder_previous[side*2];s.encoders[1]=e.encoder_previous[side*2+1];s.valid=1;s.sequence=e.seen_sequence[side]+1;s.timestamp_us=now;for(unsigned i=0;i<7;i++)s.raw[i]=raw;c77_update(&e,side,&s,now);}c77_tick(&e,now);}
static void setup(bool calibrated){c77_config c;c77_defaults(&c);if(calibrated)for(unsigned i=0;i<14;i++){c.keys[i].released=3000;c.keys[i].pressed=1000;c.keys[i].flags=7;}c77_engine_init(&e,&c,0,false);now=100;fresh(3000);}
static void enable(void){uint8_t a=1,r[64];assert(command(6,&a,1,r)==0&&e.output);}
static void test_both_polarities(void){
    rt_config_t a,b;rt_config_defaults(&a,1000,3000);rt_config_defaults(&b,3000,1000);
    rt_key_t ka,kb;assert(rt_key_configure(&ka,&a)&&rt_key_configure(&kb,&b));
    const uint16_t inputs[]={1000,1200,1500,2600,2200,2500,1300,1000};
    for(unsigned i=0;i<sizeof inputs/sizeof inputs[0];i++){
        rt_result_t ra=rt_key_process(&ka,inputs[i]),rb=rt_key_process(&kb,(uint16_t)(4000-inputs[i]));
        assert(ra.travel==rb.travel&&ra.pressed==rb.pressed&&ra.changed==rb.changed);
    }
}
static void test_bottom_deadzone(void){
    for(unsigned reverse=0;reverse<2;reverse++){
        rt_config_t cfg;rt_config_defaults(&cfg,reverse?2000:1000,reverse?1000:2000);
        cfg.bottom_deadzone=100;cfg.rt_release=50;cfg.rt_press=50;
        rt_key_t key;assert(rt_key_configure(&key,&cfg));
        const uint16_t travel[]={0,200,940,1000,910,900,899,880,851,850,860,899,900,1000,0,199,200};
        const bool down[]={false,true,true,true,true,true,true,true,true,false,false,false,true,true,false,false,true};
        bool previous=false;
        for(unsigned i=0;i<sizeof travel/sizeof travel[0];i++){
            uint16_t adc=(uint16_t)(reverse?2000-travel[i]:1000+travel[i]);
            rt_result_t result=rt_key_process(&key,adc);
            assert(key.travel==travel[i]);
            assert(result.travel==(travel[i]>=900U?1000U:travel[i])&&result.pressed==down[i]&&result.changed==(down[i]!=previous));
            previous=down[i];
        }
        /* No RT: the bottom zone does not alter the ordinary actuation rule. */
        cfg.rapid_trigger_enabled=false;assert(rt_key_configure(&key,&cfg));
        assert(rt_key_process(&key,reverse?1000:2000).pressed);
        assert(!rt_key_process(&key,reverse?1801:1199).pressed);
        cfg.enabled=false;assert(rt_key_configure(&key,&cfg));
        assert(!rt_key_process(&key,reverse?1000:2000).pressed);
        cfg.bottom_deadzone=801;assert(!rt_key_configure(&key,&cfg));
        cfg.bottom_deadzone=800;assert(rt_key_configure(&key,&cfg));
    }
    /* Zero bottom zone retains the original peak-based release at full depth. */
    rt_config_t cfg;rt_config_defaults(&cfg,1000,2000);rt_key_t key;
    assert(cfg.bottom_deadzone==0&&rt_key_configure(&key,&cfg));
    assert(rt_key_process(&key,2000).pressed);
    assert(rt_key_process(&key,1951).pressed);
    assert(!rt_key_process(&key,1950).pressed);
}
static void test_bottom_config_compatibility(void){
    c77_config a,b;c77_defaults(&a);a.keys[0].released=3000;a.keys[0].pressed=1000;a.keys[0].flags=7;
    uint8_t blob[C77_CONFIG_BYTES],legacy[C77_CONFIG_BYTES];c77_config_encode(&a,legacy);
    for(unsigned i=0;i<14;i++)for(unsigned j=16;j<22;j++)assert(legacy[i*22+j]==0);
    assert(c77_config_decode(&b,legacy)&&b.keys[0].bottom_deadzone==0&&b.keys[0].curve_id==0&&b.keys[0].released==3000);
    a.keys[0].bottom_deadzone=100;c77_config_encode(&a,blob);
    assert(blob[16]==100&&blob[17]==0&&c77_config_decode(&b,blob)&&b.keys[0].bottom_deadzone==100);
    blob[16]=0;assert(!memcmp(blob,legacy,C77_CONFIG_BYTES));
    blob[18]=1;assert(c77_config_decode(&b,blob)&&b.keys[0].curve_id==1&&b.keys[0].released==3000&&b.keys[0].pressed==1000);
    c77_config_encode(&b,blob);assert(blob[18]==1&&blob[19]==0&&blob[20]==0&&blob[21]==0);
    blob[18]=2;assert(!c77_config_decode(&b,blob));
    blob[18]=0;blob[20]=1;assert(!c77_config_decode(&b,blob));
    a.keys[0].bottom_deadzone=801;assert(!c77_config_valid(&a));
}
static void test_reference_curve(void){
    rt_config_t a,b;rt_config_defaults(&a,2206,2634);rt_config_defaults(&b,2634,2206);
    assert(a.curve_id==0&&b.curve_id==0);
    rt_key_t ka,kb;
    /* The legacy map must not change for any 12-bit input. */
    assert(rt_key_configure(&ka,&a));
    for(unsigned raw=0;raw<4096;raw++){
        unsigned expected=raw<=2206?0:raw>=2634?1000:((raw-2206)*1000+214)/428;
        (void)rt_key_process(&ka,(uint16_t)raw);
        assert(ka.travel==expected);
    }
    a.curve_id=b.curve_id=RT_CURVE_OWLAB_TI_REFERENCE;
    assert(rt_key_configure(&ka,&a)&&rt_key_configure(&kb,&b));
    uint16_t forward[4096],previous=0;
    for(unsigned raw=0;raw<4096;raw++){
        rt_result_t ra=rt_key_process(&ka,(uint16_t)raw);
        rt_result_t rb=rt_key_process(&kb,(uint16_t)(4840-raw));
        assert(ra.travel>=previous&&ra.travel<=1000&&ra.travel==rb.travel);
        assert(ra.pressed==rb.pressed&&ra.changed==rb.changed);
        forward[raw]=ra.travel;previous=ra.travel;
    }
    assert(forward[0]==0&&forward[2206]==0&&forward[2634]==1000&&forward[4095]==1000);
    /* Travel does not depend on whether the switch is pressing or releasing. */
    for(int raw=4095;raw>=0;raw--)assert(rt_key_process(&ka,(uint16_t)raw).travel==forward[raw]);
    /* Exact table nodes independently verify all 8 segments' end values. */
    rt_config_defaults(&a,0,10000);a.curve_id=RT_CURVE_OWLAB_TI_REFERENCE;
    assert(rt_key_configure(&ka,&a));
    const uint16_t signal[]={0,467,1211,2244,3667,5644,8500,10000};
    const uint16_t expected[]={0,152,303,455,606,758,909,1000};
    for(unsigned i=0;i<8;i++)assert(rt_key_process(&ka,signal[i]).travel==expected[i]);
    /* Rounding and intermediate interpolation, not just literal table nodes. */
    assert(rt_key_process(&ka,800).travel==219);
    assert(rt_key_process(&ka,3000).travel==535);
    /* Initial actuation and RT use corrected distance, not the old ADC ratio. */
    a.rt_press=a.rt_release=100;assert(rt_key_configure(&ka,&a));
    assert(rt_key_process(&ka,1211).pressed); /* 303 vs old linear 121. */
    assert(rt_key_process(&ka,2244).pressed);
    assert(!rt_key_process(&ka,1211).pressed);
    assert(!rt_key_process(&ka,1800).pressed);
    assert(rt_key_process(&ka,2244).pressed);
    assert(!rt_key_process(&ka,0).pressed);
    a.bottom_deadzone=100;assert(rt_key_configure(&ka,&a));
    assert(rt_key_process(&ka,10000).pressed);
    assert(rt_key_process(&ka,8000).pressed);
    assert(!rt_key_process(&ka,5644).pressed);
    a.enabled=false;assert(rt_key_configure(&ka,&a));
    assert(!rt_key_process(&ka,10000).pressed&&ka.travel==1000);
    a.curve_id=2;assert(!rt_key_configure(&ka,&a));
}
static void test_rt_and_report(void){
    setup(true);enable();now+=63;fresh(2500);assert(e.pressed_mask&1);uint8_t keys[32];uint16_t consumer;c77_report(&e,keys,&consumer);assert(keys[0x2b/8]&(1u<<(0x2b%8)));assert(keys[0xe1/8]&(1u<<(0xe1%8)));
    now+=63;fresh(2800);assert(!(e.pressed_mask&1));now+=63;fresh(2550);assert(e.pressed_mask&1);
    now+=63;fresh(3000);assert(!(e.pressed_mask&1));
    now+=5001;c77_tick(&e,now);assert(!e.output);c77_report(&e,keys,&consumer);for(unsigned i=0;i<32;i++)assert(keys[i]==0);
    /* Wrap-safe freshness and no processing repeated sequence. */
    setup(true);now=0xfffffff0u;fresh(3000);now=40;assert(c77_source_fresh(&e,0,now));
    c77_sample_t same=e.sample[0];same.raw[0]=1000;same.timestamp_us=now;c77_update(&e,0,&same,now);assert(!e.travel[0]);
    setup(true);enable();e.seen_sequence[1]=100;e.encoder_previous[2]=10;
    c77_sample_t reboot=e.sample[1];reboot.sequence=1;reboot.timestamp_us=now;reboot.encoders[0]=0;c77_update(&e,1,&reboot,now);assert(!e.output&&!e.wheel_pending);
    setup(true);enable();now+=5001;fresh(3000);assert(!e.output); /* Never bridge a stale interval. */
}
static void test_commands(void){
    setup(false);uint8_t r[64],one=1;assert(command(6,&one,1,r)==C77_NOT_READY);assert(command(1,NULL,0,r)==0&&r[7]==34&&r[12]==14);
    assert(r[38]==HARDWARE_REVISION&&r[39]==1&&r[13]==6&&r[14]==4&&r[15]==28);
    assert(r[8]==0&&r[9]==3&&r[10]==(FW_UPDATE_VERSION&0xffu));
    assert(r[25]==(HARDWARE_REVISION==86?0x63:0x43));
    assert(!e.output); /* INFO and initial connection never enable input. */
    uint8_t q[64],bad[64];packet(q,1,NULL,0);for(unsigned i=2;i<64;i++)for(unsigned b=0;b<8;b++){memcpy(bad,q,64);bad[i]^=(uint8_t)(1u<<b);assert(c77_command(&e,bad,r,now,2,1024)&&r[6]==C77_BAD_PACKET);}
    uint8_t index=14;assert(command(3,&index,1,r)==C77_BAD_INDEX);index=0;assert(command(3,&index,1,r)==0);uint8_t key[22];memcpy(key,r+8,22);key[18]=2;assert(command(4,key,22,r)==C77_BAD_CONFIG);
    setup(true);enable();index=0;assert(command(3,&index,1,r)==0);memcpy(key,r+8,22);key[6]=0xfa;key[7]=0;assert(command(4,key,22,r)==0&&!e.output&&e.dirty&&e.config.keys[0].actuation==250);
    key[16]=100;assert(command(4,key,22,r)==0&&r[24]==100&&e.config.keys[0].bottom_deadzone==100&&e.keys[0].config.bottom_deadzone==100);
    key[18]=1;assert(command(4,key,22,r)==0&&r[26]==1&&e.config.keys[0].curve_id==1&&e.keys[0].config.curve_id==1&&!e.output);
    assert(e.config.keys[0].released==3000&&e.config.keys[0].pressed==1000);
    now+=63;fresh(2758);assert(e.travel[0]==303); /* 1210/10000 signal; rounded travel303. */
    uint8_t side=0;assert(command(2,&side,1,r)==0&&r[38]==47&&r[39]==1); /* STATUS travel303. */
    uint8_t bind[6]={20,2,1,0,0,0};assert(command(10,bind,6,r)==0);bind[0]=0;assert(command(10,bind,6,r)==0);
    bind[1]=5;bind[2]=5;assert(command(10,bind,6,r)==0);bind[2]=6;assert(command(10,bind,6,r)==C77_BAD_CONFIG);
    assert(command(7,NULL,0,r)==0&&e.save_requested&&!e.output&&!e.save_output);assert(command(8,NULL,0,r)==C77_BUSY);
    setup(true);enable();
    assert(command(7,NULL,0,r)==0&&e.save_requested&&!e.output&&e.save_output);
    uint8_t zero=0;assert(command(6,&zero,1,r)==0&&!e.save_output);
    setup(true);e.restore_output_pending=true;now+=5001;
    assert(!c77_enable_output(&e,now)&&!e.output&&e.restore_output_pending);
    fresh(3000);assert(c77_enable_output(&e,now)&&e.output&&!e.restore_output_pending);
}
static void test_calibration(void){
    setup(false);uint8_t p[2]={0,0},r[64];assert(command(5,p,2,r)==0&&e.cal_state==1);
    for(unsigned i=0;i<256;i++){now+=63;fresh(3000);}assert(e.cal_state==2&&e.cal_rest==3000);
    p[1]=1;assert(command(5,p,2,r)==0);for(unsigned i=0;i<256;i++){now+=63;fresh(1000);}assert(e.cal_state==4&&e.cal_pressed==1000);
    p[1]=2;assert(command(5,p,2,r)==0&&e.config.keys[0].flags==7&&e.dirty&&!e.output);
    assert(e.config.keys[0].released==3000&&e.config.keys[0].pressed==1000);
    p[1]=0;assert(command(5,p,2,r)==0);for(unsigned i=0;i<256;i++){now+=63;fresh((uint16_t)(i%2?3000:2800));}assert(!e.cal_state&&e.cal_key==255);assert(e.config.keys[0].released==3000);
    p[1]=0;assert(command(5,p,2,r)==0);now+=5000001;c77_tick(&e,now);assert(!e.cal_state);
}
static void test_encoder_and_debounce(void){
    setup(true);
    /* Exercise scroll and consumer pulse behavior independently of the
     * user's mapping baseline, which now maps these directions to keys. */
    e.config.bindings[20]=(c77_binding){.kind=2,.code=1};
    e.config.bindings[22]=(c77_binding){.kind=4,.code=0xe9};
    enable();c77_sample_t s=e.sample[0];s.sequence++;s.timestamp_us=now;s.encoders[0]=3;s.digital=1;c77_update(&e,0,&s,now);
    int8_t wheel,pan;c77_mouse_peek(&e,&wheel,&pan);assert(wheel==3&&pan==0);c77_mouse_accepted(&e,wheel,pan);assert(!e.wheel_pending);
    assert(!(e.pressed_mask&(1u<<14)));now+=3001;s.sequence++;s.timestamp_us=now;c77_update(&e,0,&s,now);assert(e.pressed_mask&(1u<<14));
    s.sequence++;s.encoders[1]=1;c77_update(&e,0,&s,now);c77_tick(&e,now);uint8_t keys[32];uint16_t consumer;c77_report(&e,keys,&consumer);assert(consumer==0xe9);c77_consumer_acked(&e,now);now+=2100;fresh(3000);c77_report(&e,keys,&consumer);assert(!consumer);c77_consumer_acked(&e,now);assert(!e.pulse[2]);
}
static void test_nkro_and_mouse(void){
    setup(true);enable();uint8_t keys[32];uint16_t consumer;
    for(unsigned i=0;i<20;i++)e.config.bindings[i]=(c77_binding){.kind=1,.code=(uint16_t)(4+i)};
    e.config.bindings[0].modifiers=1;e.config.bindings[19].modifiers=128;
    e.pressed_mask=(1u<<20)-1;c77_report(&e,keys,&consumer);
    /* Every source is retained, including >6 distinct keys plus modifiers. */
    for(unsigned i=4;i<24;i++)assert(keys[i/8]&(1u<<(i%8)));
    assert(keys[0xe0/8]==0x81);
    e.config.bindings[1].code=4;e.pressed_mask=3;c77_report(&e,keys,&consumer);assert(keys[0]&16);
    e.pressed_mask=2;c77_report(&e,keys,&consumer);assert(keys[0]&16);
    e.pressed_mask=0;c77_report(&e,keys,&consumer);assert(!(keys[0]&16));
    for(unsigned i=0;i<5;i++)e.config.bindings[i]=(c77_binding){.kind=5,.code=(uint16_t)(i+1)};
    e.pressed_mask=31;assert(c77_mouse_buttons(&e)==31);
    e.config.bindings[5]=(c77_binding){.kind=5,.code=1};e.pressed_mask=(1u<<5)|1;
    assert(c77_mouse_buttons(&e)==1);e.pressed_mask=1u<<5;assert(c77_mouse_buttons(&e)==1);
    e.pressed_mask=0;assert(c77_mouse_buttons(&e)==0);
    e.config.bindings[20]=(c77_binding){.kind=5,.code=2};e.pulse[0]=1;
    assert(c77_mouse_buttons(&e)==2);c77_mouse_acked(&e,2,now);assert(e.pulse[0]==2);
    now+=2001;c77_tick(&e,now);assert(e.pulse[0]==3&&c77_mouse_buttons(&e)==0);
    c77_mouse_acked(&e,0,now);assert(!e.pulse[0]);
    e.pressed_mask=31;c77_disable(&e);assert(!c77_mouse_buttons(&e));
    enable();e.pressed_mask=31;now+=5001;c77_tick(&e,now);assert(!e.output&&!c77_mouse_buttons(&e));
    /* Ordinary Hall press -> one scroll tick; hold does not repeat; RT
     * release/repress adds one more tick. No RT behavior for encoder inputs. */
    setup(true);e.config.bindings[0]=(c77_binding){.kind=2,.code=1};enable();
    now+=63;fresh(2500);assert(e.wheel_pending==1);now+=63;fresh(2500);assert(e.wheel_pending==1);
    now+=63;fresh(2800);now+=63;fresh(2550);assert(e.wheel_pending==2);
}
static void test_keyboard_completed_report(void){
    setup(true);enable();uint8_t old_report[32]={0},actual[32];uint16_t consumer;
    e.config.bindings[20]=(c77_binding){.kind=1,.code=4,.modifiers=3};
    e.config.bindings[21]=(c77_binding){.kind=1,.code=5};
    e.pulse[0]=e.pulse[1]=1;
    /* An empty older IN completion cannot consume either new pulse. */
    c77_keyboard_report_acked(&e,old_report,now);assert(e.pulse[0]==1&&e.pulse[1]==1);
    c77_report(&e,actual,&consumer);actual[0xe0/8]=1; /* One modifier still missing. */
    c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==1&&e.pulse[1]==2);
    c77_report(&e,actual,&consumer);actual[4/8]&=0xefU;
    c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==1); /* Modifiers alone are insufficient. */
    c77_report(&e,actual,&consumer);c77_keyboard_report_acked(&e,actual,now);
    assert(e.pulse[0]==2&&e.pulse_started[0]==now);
    memcpy(old_report,actual,32);now+=2001;c77_tick(&e,now);assert(e.pulse[0]==3&&e.pulse[1]==3);
    c77_keyboard_report_acked(&e,old_report,now);assert(e.pulse[0]==3&&e.pulse[1]==3);
    memset(actual,0,32);actual[0xe0/8]=1; /* Another held source shares a modifier. */
    c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==3&&e.pulse[1]==0);
    actual[0xe0/8]=0;actual[4/8]=(uint8_t)(1u<<(4%8));
    c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==3);
    memset(actual,0,32);c77_keyboard_report_acked(&e,actual,now);assert(!e.pulse[0]);
    e.pulse[0]=1;c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==1);
    c77_keyboard_report_acked(&e,NULL,now);assert(e.pulse[0]==1);
    c77_keyboard_acked(&e,now);assert(e.pulse[0]==2); /* Synchronous compatibility wrapper. */
    e.config.bindings[20]=(c77_binding){.kind=1,.code=0,.modifiers=0x80};e.pulse[0]=1;
    memset(actual,0,32);c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==1);
    actual[0xe0/8]=0x80;c77_keyboard_report_acked(&e,actual,now);assert(e.pulse[0]==2);
}
static void test_key_chord(void){
    setup(true);
    uint8_t reply[64];
    const uint8_t chord[6]={0,C77_BINDING_KEY_CHORD,4,5,2,7};
    const uint8_t legacy_with_extra[6]={0,1,4,0,2,7};
    assert(command(10,legacy_with_extra,6,reply)==C77_BAD_CONFIG);
#if HARDWARE_REVISION == 86
    assert(command(10,chord,6,reply)==C77_OK&&!memcmp(reply+8,chord,6));
    const uint8_t invalid[][6]={
        {0,C77_BINDING_KEY_CHORD,3,5,2,7},
        {0,C77_BINDING_KEY_CHORD,5,4,2,0},
        {0,C77_BINDING_KEY_CHORD,4,4,2,0},
        {0,C77_BINDING_KEY_CHORD,4,0xe0,2,0},
        {0,C77_BINDING_KEY_CHORD,4,5,2,5},
        {0,C77_BINDING_KEY_CHORD,4,5,2,0xe1}
    };
    for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)
        assert(command(10,invalid[i],6,reply)==C77_BAD_CONFIG);
    assert(command(9,chord,1,reply)==C77_OK&&!memcmp(reply+8,chord,6));
    uint8_t encoded[C77_CONFIG_BYTES];c77_config saved;
    c77_config_encode(&e.config,encoded);
    assert(c77_config_decode(&saved,encoded));
    assert(saved.bindings[0].code==0x0504&&saved.bindings[0].third_key==7);

    /* A shared key and modifier remain held when the chord source releases. */
    const uint8_t single[6]={1,1,5,0,2,0};
    const uint8_t encoder_chord[6]={20,C77_BINDING_KEY_CHORD,4,7,2,0};
    assert(command(10,single,6,reply)==C77_OK);
    assert(command(10,encoder_chord,6,reply)==C77_OK);
    enable();
    uint8_t report[32];uint16_t consumer;
    c77_sample_t local=e.sample[0];
    local.sequence++;local.raw[0]=2500;local.raw[1]=2500;
    c77_update(&e,0,&local,now);
    c77_report(&e,report,&consumer);
    assert(report[0]==0xb0&&report[0xe0/8]==2);
    local.sequence++;local.raw[0]=3000;
    c77_update(&e,0,&local,now);
    c77_report(&e,report,&consumer);
    assert(report[0]==0x20&&report[0xe0/8]==2);
    local.sequence++;local.raw[1]=3000;local.encoders[0]=1;
    c77_update(&e,0,&local,now);c77_tick(&e,now);
    c77_report(&e,report,&consumer);
    assert(e.pulse[0]==1&&report[0]==0x90&&report[0xe0/8]==2);
    report[0]&=0x7f;
    c77_keyboard_report_acked(&e,report,now);
    assert(e.pulse[0]==1); /* A completion missing one chord key is insufficient. */
    c77_keyboard_acked(&e,now);
    assert(e.pulse[0]==2);
    now+=2000;c77_tick(&e,now);
    assert(e.pulse[0]==3);
    memset(report,0,32);report[0]=0x80;
    c77_keyboard_report_acked(&e,report,now);
    assert(e.pulse[0]==3); /* A surviving chord key delays release ACK. */
    c77_keyboard_acked(&e,now);
    assert(e.pulse[0]==0);
#else
    assert(command(10,chord,6,reply)==C77_BAD_CONFIG);
#endif
}
static void test_acquisition_filter_math(void)
{
    for(unsigned window=2U;window<=8U;window*=4U) {
        hall_filter_t filter;hall_filter_reset(&filter);
        uint16_t raw[7],out[7];
        for(unsigned j=0;j<7;j++) raw[j]=(uint16_t)(2200U+j);
        assert(hall_filter_push(&filter,raw,out,window));
        assert(!memcmp(raw,out,sizeof raw));
        for(unsigned i=0;i<64U;i++) {
            for(unsigned j=0;j<7;j++) raw[j]=(uint16_t)(2200U+j+(i%2U?16:-16));
            assert(hall_filter_push(&filter,raw,out,window));
            if(i+1U>=window) for(unsigned j=0;j<7;j++) assert(out[j]==2200U+j);
        }
        /* Both ADC extremes fit the sums; full steps settle in exactly the
         * selected number of new scans without overshoot or stale history. */
        hall_filter_reset(&filter);memset(raw,0,sizeof raw);
        assert(hall_filter_push(&filter,raw,out,window));
        uint16_t previous=0U;
        for(unsigned i=0;i<window;i++) {
            for(unsigned j=0;j<7;j++) raw[j]=4095U;
            assert(hall_filter_push(&filter,raw,out,window));
            assert(out[0]>=previous && out[0]<=4095U);previous=out[0];
            if(i+1U<window) assert(out[0]<4095U);
        }
        assert(out[0]==4095U);
        memset(raw,0,sizeof raw);
        for(unsigned i=0;i<window;i++) {
            assert(hall_filter_push(&filter,raw,out,window));
            assert(out[0]<=previous);previous=out[0];
        }
        assert(out[0]==0U);
        raw[2]=4096U;assert(!hall_filter_push(&filter,raw,out,window) && !filter.ready);
        for(unsigned j=0;j<7;j++) raw[j]=1234U;
        assert(hall_filter_push(&filter,raw,out,window) && out[0]==1234U);
    }
}
static void test_conditioned_engine_path(void)
{
    setup(true);
    c77_sample_t sample=e.sample[0];sample.sequence++;sample.timestamp_us=++now;
    sample.hall_filtered_valid=1U;
    for(unsigned i=0;i<7;i++){sample.raw[i]=1000U;sample.hall_filtered[i]=3000U;}
    c77_update(&e,0,&sample,now);
    uint8_t side=0,r[64];assert(command(2,&side,1,r)==C77_OK);
    assert((r[24]|((uint16_t)r[25]<<8))==1000U); /* STATUS remains raw. */
#if HARDWARE_REVISION==86
    assert(e.travel[0]==0U && !(e.pressed_mask&1U));
    assert((r[38]|((uint16_t)r[39]<<8))==0U); /* Decision uses average. */
#else
    assert(e.travel[0]==1000U && (e.pressed_mask&1U));
#endif
    /* A right-side sample can never opt into the private left filter. */
    sample.sequence=e.seen_sequence[1]+1U;sample.timestamp_us=++now;
    c77_update(&e,1,&sample,now);
    assert(e.travel[7]==1000U && (e.pressed_mask&(1U<<7)));
    /* Raw out-of-range remains invalid even if the average looks normal. */
    sample.sequence=e.seen_sequence[0]+1U;sample.timestamp_us=++now;sample.raw[0]=5000U;
    c77_update(&e,0,&sample,now);c77_tick(&e,now);
    assert(!c77_source_fresh(&e,0,now) && !(e.pressed_mask&0x7fU));
#if HARDWARE_REVISION==86
    setup(true);sample=e.sample[0];sample.sequence++;sample.timestamp_us=++now;
    sample.hall_filtered_valid=1U;
    for(unsigned i=0;i<7;i++) sample.hall_filtered[i]=3000U;
    sample.hall_filtered[0]=5000U;
    c77_update(&e,0,&sample,now);c77_tick(&e,now);
    assert(!c77_source_fresh(&e,0,now) && !(e.pressed_mask&0x7fU));
    /* Hardware calibration averages original measurements, never the private
     * already-filtered data. Existing endpoint semantics stay unchanged. */
    setup(true);e.cal_key=0;e.cal_state=1;e.cal_started=now;
    sample=e.sample[0];sample.hall_filtered_valid=1U;
    for(unsigned i=0;i<7;i++){sample.raw[i]=3170U;sample.hall_filtered[i]=3000U;}
    for(unsigned i=0;i<256;i++) {
        sample.sequence=e.seen_sequence[0]+1U;sample.timestamp_us=++now;
        c77_update(&e,0,&sample,now);
    }
    assert(e.cal_state==2 && e.cal_rest==3170U);
#endif
}
static void test_fractional_filter(void)
{
    hall_filter_t f;hall_filter_reset(&f);
    uint16_t raw14[7];uint32_t q5[7];
    for(unsigned j=0;j<7;j++)raw14[j]=(uint16_t)(8800U+j*4U);
    assert(hall_filter_push14_q5(&f,raw14,q5));
    for(unsigned j=0;j<7;j++)assert(q5[j]==(2200U+j)*32U);
    /* Each added14-bit LSB changes the8-scan sum by one Q5 unit. The old
     * integer ADC average would lose all eight intermediate fractional steps. */
    raw14[0]=8801U;
    for(unsigned i=1;i<=8;i++){
        assert(hall_filter_push14_q5(&f,raw14,q5));
        assert(q5[0]==70400U+i && q5[6]==2206U*32U);
    }
    raw14[0]=8800U;
    for(unsigned i=1;i<=8;i++){
        assert(hall_filter_push14_q5(&f,raw14,q5));
        assert(q5[0]==70408U-i);
    }
    /* Exact extremes and a full-scale alternating signal never overflow. */
    hall_filter_reset(&f);memset(raw14,0,sizeof raw14);
    assert(hall_filter_push14_q5(&f,raw14,q5) && q5[0]==0U);
    for(unsigned i=0;i<8;i++){
        for(unsigned j=0;j<7;j++)raw14[j]=16383U;
        assert(hall_filter_push14_q5(&f,raw14,q5));
        assert(q5[0]==(i+1U)*16383U);
    }
    assert(q5[0]==RT_ADC_Q5_MAX);
    for(unsigned i=0;i<32;i++){
        for(unsigned j=0;j<7;j++)raw14[j]=(uint16_t)(i%2U?16383U:0U);
        assert(hall_filter_push14_q5(&f,raw14,q5));
        if(i>=7U)assert(q5[0]==65532U);
    }
    raw14[6]=16384U;assert(!hall_filter_push14_q5(&f,raw14,q5) && !f.ready);
    for(unsigned j=0;j<7;j++)raw14[j]=8001U;
    assert(hall_filter_push14_q5(&f,raw14,q5) && q5[0]==64008U);
    f.sum[0]=UINT32_MAX;
    assert(!hall_filter_push14_q5(&f,raw14,q5) && !f.ready);
    assert(hall_filter_push14_q5(&f,raw14,q5));f.sum[0]=0U;
    assert(!hall_filter_push14_q5(&f,raw14,q5) && !f.ready);
    assert(hall_filter_push14_q5(&f,raw14,q5));f.next=8U;
    assert(!hall_filter_push14_q5(&f,raw14,q5) && !f.ready);
    assert(!hall_filter_push14_q5(NULL,raw14,q5));
}
static void assert_rt_equal(rt_result_t a,rt_result_t b)
{
    assert(a.travel==b.travel && a.pressed==b.pressed && a.changed==b.changed);
}
static void test_fractional_rt(void)
{
    const uint16_t endpoints[][2]={{1000,1400},{1400,1000},{2206,2634},
        {2634,2206},{0,65535},{65535,0},{0,128},{128,0}};
    for(unsigned curve=0;curve<=1;curve++)for(unsigned pair=0;pair<8;pair++){
        rt_config_t cfg;rt_config_defaults(&cfg,endpoints[pair][0],endpoints[pair][1]);
        cfg.curve_id=(uint16_t)curve;cfg.bottom_deadzone=100;
        rt_key_t whole,fraction;assert(rt_key_configure(&whole,&cfg));
        assert(rt_key_configure(&fraction,&cfg));
        /* Includes generic uint16 endpoints far beyond physical ADC range;
         * inverted65535..0 exercises the64-bit intermediate slow path. */
        for(unsigned raw=0;raw<4096;raw++)
            assert_rt_equal(rt_key_process(&whole,(uint16_t)raw),
                rt_key_process_q5(&fraction,raw*32U));
        for(int raw=4095;raw>=0;raw--)
            assert_rt_equal(rt_key_process(&whole,(uint16_t)raw),
                rt_key_process_q5(&fraction,(uint32_t)raw*32U));
    }
    for(unsigned curve=0;curve<=1;curve++)for(unsigned reverse=0;reverse<2;reverse++){
        const uint16_t low=(uint16_t)(curve?2203:1000),high=(uint16_t)(curve?2628:1400);
        const uint32_t mirror=((uint32_t)low+high)*32U;
        rt_config_t cfg;rt_config_defaults(&cfg,reverse?high:low,reverse?low:high);
        cfg.curve_id=(uint16_t)curve;
        cfg.rt_press=cfg.rt_release=100;cfg.top_deadzone=30;
        rt_key_t key;assert(rt_key_configure(&key,&cfg));
        /* Fractional samples on either side of the exact rounded-distance
         * boundaries. Adjacent samples34553/34554 both round to whole ADC1080,
         * but only the latter must actuate. Reverse polarity is identical. */
        const uint32_t linear[]={32000,34553,34554,38400,37127,37126,38393,38394,32391,32390};
        const uint32_t owlab[]={70496,71451,71452,74128,73047,73046,74122,74123,70625,70624};
        const uint32_t *input=curve?owlab:linear;
        const uint16_t travel[]={0,199,200,500,401,400,499,500,31,30};
        const bool down[]={false,false,true,true,true,false,false,true,false,false};
        bool last=false;
        for(unsigned i=0;i<10;i++){
            uint32_t q5=reverse?mirror-input[i]:input[i];
            rt_result_t v=rt_key_process_q5(&key,q5);
            assert(key.travel==travel[i]);
            assert(v.travel==(travel[i]<=30U?0U:travel[i]) && v.pressed==down[i] && v.changed==(down[i]!=last));
            last=down[i];
        }
        assert(!key.rapid_cycle);
        /* Invalid fractional data must release rather than clamp as a press. */
        assert(rt_key_process_q5(&key,(uint32_t)(reverse?low:high)*32U).pressed);
        rt_result_t bad=rt_key_process_q5(&key,RT_ADC_Q5_MAX+1U);
        assert(!bad.pressed && bad.changed && !key.rapid_cycle && key.valid);
        bad=rt_key_process_q5(&key,UINT32_MAX);assert(!bad.pressed && !bad.changed);
    }
    /* Every fractional input in the physical range remains monotonic. */
    for(unsigned curve=0;curve<=1;curve++){
        rt_config_t cfg;rt_config_defaults(&cfg,2206,2634);cfg.curve_id=(uint16_t)curve;
        rt_key_t key;assert(rt_key_configure(&key,&cfg));uint16_t previous=0;
        for(uint32_t q5=0;q5<=RT_ADC_Q5_MAX;q5++){
            rt_result_t r=rt_key_process_q5(&key,q5);
            assert(r.travel>=previous && r.travel<=1000);previous=r.travel;
        }
        for(uint32_t q5=RT_ADC_Q5_MAX;q5>0;q5--){
            rt_result_t r=rt_key_process_q5(&key,q5);
            assert(r.travel<=previous);previous=r.travel;
        }
    }
    assert(!rt_key_process_q5(NULL,0).pressed);
}
static void assert_rt_state_equal(const rt_key_t *a,const rt_key_t *b)
{
    assert(a->travel==b->travel && a->extreme==b->extreme && a->pressed==b->pressed);
    assert(a->rapid_cycle==b->rapid_cycle && a->valid==b->valid);
}
static void test_normalization_cache(void)
{
    /* Compare the cached path with recomputation on every call. State is not
     * reset in the reference, so press/release edges and RT extrema must match. */
    const uint16_t endpoints[][2]={{1000,2000},{2000,1000},{0,65535},{65535,0}};
    const uint16_t inputs[]={0,1000,1100,1200,1800,2000,1980,1900,1950,1500,1200,1000,65535};
    for(unsigned curve=0;curve<2;curve++)for(unsigned pair=0;pair<4;pair++)
    for(unsigned mode=0;mode<2;mode++)for(unsigned flags=0;flags<4;flags++) {
        rt_config_t cfg;rt_config_defaults(&cfg,endpoints[pair][0],endpoints[pair][1]);
        cfg.curve_id=(uint16_t)curve;cfg.bottom_deadzone=100;
        cfg.enabled=(flags&1U)!=0;cfg.rapid_trigger_enabled=(flags&2U)!=0;
        rt_key_t cached,reference;
        assert(rt_key_configure(&cached,&cfg)&&rt_key_configure(&reference,&cfg));
        for(unsigned i=0;i<sizeof inputs/sizeof inputs[0];i++)for(unsigned repeat=0;repeat<4;repeat++) {
            reference.normalization_kind=0; /* Force the uncached normalization. */
            rt_result_t a,b;
            if(mode) {
                uint32_t value=(uint32_t)inputs[i]*32U+1U;
                a=rt_key_process_q5(&cached,value);b=rt_key_process_q5(&reference,value);
            } else {
                a=rt_key_process(&cached,inputs[i]);b=rt_key_process(&reference,inputs[i]);
            }
            assert_rt_equal(a,b);assert_rt_state_equal(&cached,&reference);
            if(repeat)assert(!a.changed); /* Never replay the first cached edge. */
        }
        rt_key_reset(&cached);rt_key_reset(&reference);
        assert(!cached.normalization_kind && !reference.normalization_kind);
        assert_rt_equal(rt_key_process(&cached,1800),rt_key_process(&reference,1800));
        assert_rt_state_equal(&cached,&reference);
        /* Same numeric input after changing endpoint polarity, curve, enable,
         * thresholds and deadzones must follow the new configuration. */
        cfg.adc_released=2000;cfg.adc_pressed=1000;cfg.curve_id=(uint16_t)(1U-curve);
        cfg.actuation=350;cfg.rt_press=40;cfg.rt_release=70;cfg.top_deadzone=50;
        cfg.bottom_deadzone=200;cfg.enabled=!cfg.enabled;cfg.rapid_trigger_enabled=!cfg.rapid_trigger_enabled;
        assert(rt_key_configure(&cached,&cfg)&&rt_key_configure(&reference,&cfg));
        assert(!cached.normalization_kind);
        assert_rt_equal(rt_key_process(&cached,1800),rt_key_process(&reference,1800));
        cfg.adc_pressed=cfg.adc_released;assert(!rt_key_configure(&cached,&cfg));
        assert(!cached.normalization_kind && !rt_key_process(&cached,1800).pressed);
        assert(!rt_key_configure(&cached,NULL) && !cached.normalization_kind);
    }
    /* Whole/Q5 numeric inputs have different units, even when identical. */
    rt_config_t cfg;rt_config_defaults(&cfg,0,4000);rt_key_t key;
    assert(rt_key_configure(&key,&cfg));
    assert(rt_key_process(&key,3000).travel==750);
    assert(rt_key_process_q5(&key,3000).travel==23);
    assert(rt_key_process(&key,3000).travel==750);
    assert(rt_key_process_q5(&key,3000U*32U).travel==750);
    assert(rt_key_process_q5(&key,RT_ADC_Q5_MAX).travel==1000);
    rt_result_t invalid=rt_key_process_q5(&key,RT_ADC_Q5_MAX+1U);
    assert(!invalid.pressed && invalid.changed && !key.normalization_kind);
    assert(rt_key_process_q5(&key,RT_ADC_Q5_MAX).pressed); /* First sample after fault. */
    rt_key_reset(&key);assert(!key.normalization_kind);
    assert(rt_key_process_q5(&key,RT_ADC_Q5_MAX).changed); /* First sample after reset. */
}
#if SCAN_DUAL_96K
static void test_fractional_engine_path(void)
{
    setup(true);
    c77_sample_t s=e.sample[0];s.sequence++;s.timestamp_us=++now;s.hall_q5_valid=1U;
    for(unsigned i=0;i<7;i++){s.raw[i]=1000U;s.hall_q5[i]=3000U*32U;}
    c77_update(&e,0,&s,now);
    assert(!e.travel[0] && !(e.pressed_mask&1U));
    assert(e.filtered_q5[0]==96000U && e.filtered_q5_ready[0]);
    uint8_t side=0,r[64];assert(command(2,&side,1,r)==C77_OK);
    assert((r[24]|((uint16_t)r[25]<<8))==1000U); /* STATUS stays12-bit raw. */
    /* Remote frames cannot inject private fractional data, even malformed. */
    s.hall_q5_valid=2U;s.hall_q5[0]=UINT32_MAX;
    s.sequence=e.seen_sequence[1]+1U;s.timestamp_us=++now;
    c77_update(&e,1,&s,now);assert(e.travel[7]==1000U && c77_source_fresh(&e,1,now));
    /* Missing validity flag follows the legacy path and discards stale Q5
     * deadband state; later valid Q5 re-enters from its own first sample. */
    s.hall_q5_valid=0U;s.sequence=e.seen_sequence[0]+1U;s.timestamp_us=++now;
    c77_update(&e,0,&s,now);assert(e.travel[0]==1000U && !e.filtered_q5_ready[0]);
    s.hall_q5_valid=1U;for(unsigned i=0;i<7;i++)s.hall_q5[i]=96000U;
    s.sequence=e.seen_sequence[0]+1U;s.timestamp_us=++now;
    c77_update(&e,0,&s,now);assert(!e.travel[0]);
    /* A valid average cannot launder invalid raw, flags or Q5 overflow. */
    for(unsigned fault=0;fault<3;fault++){
        setup(true);enable();s=e.sample[0];s.hall_q5_valid=1U;
        for(unsigned i=0;i<7;i++)s.hall_q5[i]=96000U;
        if(fault==0)s.raw[0]=4096U;
        else if(fault==1)s.hall_q5_valid=2U;
        else s.hall_q5[0]=RT_ADC_Q5_MAX+1U;
        s.sequence++;s.timestamp_us=++now;c77_update(&e,0,&s,now);c77_tick(&e,now);
        assert(!c77_source_fresh(&e,0,now) && !e.output && !(e.pressed_mask&0x7fU));
    }
    /* Calibrations retain their original normalized raw measurements. */
    setup(true);e.cal_key=0;e.cal_state=1;e.cal_started=now;s=e.sample[0];s.hall_q5_valid=1U;
    for(unsigned i=0;i<7;i++){s.raw[i]=3170U;s.hall_q5[i]=96000U;}
    for(unsigned i=0;i<256;i++){
        s.sequence=e.seen_sequence[0]+1U;s.timestamp_us=++now;c77_update(&e,0,&s,now);
    }
    assert(e.cal_state==2 && e.cal_rest==3170U);
    /* User noise_deadband remains in legacy ADC counts, scaled exactly32.
     * Sub-count average precision must not silently weaken that setting. */
    setup(true);s=e.sample[0];s.hall_q5_valid=1U;
    for(unsigned i=0;i<7;i++)s.hall_q5[i]=96000U;
    s.sequence++;s.timestamp_us=++now;c77_update(&e,0,&s,now);
    s.hall_q5[0]=95937U;s.sequence++;s.timestamp_us=++now;c77_update(&e,0,&s,now);
    assert(e.filtered_q5[0]==96000U);
    s.hall_q5[0]=95936U;s.sequence++;s.timestamp_us=++now;c77_update(&e,0,&s,now);
    assert(e.filtered_q5[0]==95936U);
    s.hall_q5[0]=95871U;s.sequence++;s.timestamp_us=++now;c77_update(&e,0,&s,now);
    assert(e.filtered_q5[0]==95871U); /* Fractional remainder retained. */
    uint32_t prior=e.seen_sequence[0];s.valid=0U;s.sequence++;s.timestamp_us=++now;
    c77_update(&e,0,&s,now);assert(e.seen_sequence[0]==prior);
}
#endif
#if HARDWARE_REVISION==86
static void filtered_rt_frame(hall_filter_t *filter,uint16_t raw_value)
{
    c77_sample_t sample={0};sample.valid=1U;
    for(unsigned i=0;i<7;i++)sample.raw[i]=raw_value;
    sample.hall_filtered_valid=(uint8_t)hall_filter_push(filter,sample.raw,sample.hall_filtered,8U);
    sample.sequence=e.seen_sequence[0]+1U;sample.timestamp_us=(now+=16U);
    c77_update(&e,0,&sample,now);
}
static void test_filtered_rt_response(void)
{
    c77_config config;c77_defaults(&config);
    for(unsigned i=0;i<14;i++) {
        config.keys[i].released=2200;config.keys[i].pressed=2600;
        config.keys[i].flags=7;config.keys[i].curve_id=1;
        config.keys[i].actuation=200;config.keys[i].rt_press=100;config.keys[i].rt_release=100;
    }
    c77_engine_init(&e,&config,0,false);now=100;
    hall_filter_t filter;hall_filter_reset(&filter);
    filtered_rt_frame(&filter,2200U);assert(!e.pressed_mask && e.travel[0]==0U);
    /* A synthetic single64k sample80-count spike crosses the unfiltered
     * curve's threshold, but must not produce a press through8-sample mean. */
    filtered_rt_frame(&filter,2280U);assert(e.sample[0].raw[0]==2280U && !e.pressed_mask);
    for(unsigned i=0;i<8;i++){filtered_rt_frame(&filter,2200U);assert(!e.pressed_mask);}
    assert(e.travel[0]==0U);
    for(unsigned i=0;i<8;i++) filtered_rt_frame(&filter,2600U);
    assert((e.pressed_mask&1U) && e.travel[0]==1000U);
    for(unsigned i=0;i<8;i++) filtered_rt_frame(&filter,2400U);
    assert(!(e.pressed_mask&1U) && e.keys[0].rapid_cycle); /* Release RT. */
    for(unsigned i=0;i<8;i++) filtered_rt_frame(&filter,2600U);
    assert(e.pressed_mask&1U); /* Re-press RT while still well above rest. */
    for(unsigned i=0;i<8;i++) filtered_rt_frame(&filter,2200U);
    assert(!e.pressed_mask && !e.keys[0].rapid_cycle && e.travel[0]==0U);
}
#endif
int main(void){assert(c77_crc16((const uint8_t *)"123456789",9)==0x29b1);assert(c77_crc32((const uint8_t *)"123456789",9)==0xcbf43926u);test_both_polarities();test_bottom_deadzone();test_bottom_config_compatibility();test_reference_curve();test_rt_and_report();test_commands();test_calibration();test_encoder_and_debounce();test_nkro_and_mouse();test_keyboard_completed_report();test_key_chord();test_acquisition_filter_math();test_conditioned_engine_path();test_fractional_filter();test_fractional_rt();test_normalization_cache();
#if SCAN_DUAL_96K
test_fractional_engine_path();
#endif
#if HARDWARE_REVISION==86
test_filtered_rt_response();
#endif
puts("core: protocol, corruption, RT/bottom zone/curve, legacy settings, stale/wrap, calibration, NKRO, acquisition filter and completed-report ACK PASS");return 0;}
