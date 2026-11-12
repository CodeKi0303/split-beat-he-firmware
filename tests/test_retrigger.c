/* Reuse real engine/transport stubs, not a second implementation. */
#define main existing_core_tests
#include "test_core.c"
#undef main

static void test_retrigger_machine(void){
    rt_config_t c;rt_key_t k;rt_result_t r;
    rt_config_defaults(&c,1000,2000);c.rt_press=150;c.rt_release=200;
    assert(!c.retrigger_before_release);
    assert(rt_key_configure(&k,&c));
    (void)rt_key_process(&k,1500);(void)rt_key_process(&k,1325);
    assert(!rt_key_process(&k,1475).retriggered);
    c.retrigger_before_release=true;
    for(unsigned reverse=0;reverse<2;reverse++){
        c.adc_released=reverse?2000:1000;c.adc_pressed=reverse?1000:2000;
        assert(rt_key_configure(&k,&c));
        const uint16_t travel[]={0,500,325,475,625,700,610,760,560,710,0,500};
        const bool retrigger[]={false,false,false,true,false,false,false,true,false,false,false,false};
        const bool pressed[]={false,true,true,true,true,true,true,true,false,true,false,true};
        for(unsigned i=0;i<sizeof travel/sizeof travel[0];i++){
            r=rt_key_process(&k,(uint16_t)(reverse?2000-travel[i]:1000+travel[i]));
            assert(r.retriggered==retrigger[i]&&r.pressed==pressed[i]);
            if(r.retriggered)assert(!r.changed);
        }
    }
    c.adc_released=1000;c.adc_pressed=2000;c.bottom_deadzone=100;
    assert(rt_key_configure(&k,&c));
    const uint16_t bottom[]={950,925,990,900,850,1000,900,740,890};
    for(unsigned i=0;i<sizeof bottom/sizeof bottom[0];i++){
        r=rt_key_process(&k,(uint16_t)(1000+bottom[i]));
        assert(r.pressed&&r.retriggered==(i==8));
    }
    rt_key_reset(&k);assert(!k.retrigger_armed);
    c.rapid_trigger_enabled=false;assert(rt_key_configure(&k,&c));
    (void)rt_key_process(&k,1500);(void)rt_key_process(&k,1325);
    assert(!rt_key_process(&k,1475).retriggered);
    c.rapid_trigger_enabled=true;c.bottom_deadzone=0;
    assert(rt_key_configure(&k,&c));
    (void)rt_key_process_q5(&k,1500*32);(void)rt_key_process_q5(&k,1325*32);
    r=rt_key_process_q5(&k,1475*32);assert(r.retriggered&&r.pressed&&!r.changed);
    r=rt_key_process_q5(&k,RT_ADC_Q5_MAX+1);assert(!r.pressed&&!r.retriggered&&!k.retrigger_armed);
}
static void retrigger_setup(c77_binding binding){
    c77_config c;c77_defaults(&c);
    for(unsigned i=0;i<C77_HALL;i++){
        c.keys[i].released=1000;c.keys[i].pressed=2000;c.keys[i].flags=7;
        c.keys[i].rt_press=150;c.keys[i].rt_release=200;c.keys[i].noise=0;
    }
    c.keys[0].flags=15;c.bindings[0]=binding;
    assert(c77_config_valid(&c));c77_engine_init(&e,&c,0,false);
    now=100;fresh(1000);enable();
}
static void retrigger_travel(unsigned travel){
    c77_sample_t s=e.sample[0];s.sequence++;s.timestamp_us=++now;s.raw[0]=(uint16_t)(1000+travel);
    c77_update(&e,0,&s,now);
}
static void retrigger_event(void){retrigger_travel(500);retrigger_travel(325);retrigger_travel(475);}
static void completed_phase(unsigned type){
    uint8_t report[32];uint16_t consumer;c77_report(&e,report,&consumer);
    if(type==1)report[0]=c77_mouse_buttons(&e);
    if(type==2){report[0]=(uint8_t)consumer;report[1]=(uint8_t)(consumer>>8);}
    c77_retrigger_report_acked(&e,type,report,e.retrigger_epoch,now);
}
static bool usage_on(const uint8_t report[32],unsigned usage){return (report[usage/8]&(1u<<(usage%8)))!=0;}
static void test_retrigger_output(void){
    const c77_binding binding[]={
        {.kind=1,.code=4},{.kind=C77_BINDING_KEY_CHORD,.code=0x0504,.third_key=6,.modifiers=2},
        {.kind=5,.code=1},{.kind=4,.code=0xe9}
    };
    for(unsigned b=0;b<4;b++){
        retrigger_setup(binding[b]);retrigger_event();assert(e.retrigger[0]==1&&(e.pressed_mask&1));
        unsigned type=b<2?0:b==2?1:2;
        uint8_t old[32]={0};uint32_t stale=e.retrigger_epoch-1;
        /* Arbitrarily delayed/old completion cannot acknowledge new intent. */
        c77_retrigger_report_acked(&e,type,old,stale,now);assert(e.retrigger[0]==1);
        completed_phase(type);assert(e.retrigger[0]==2&&(e.pressed_mask&1));
        c77_retrigger_report_acked(&e,type,old,stale,now);assert(e.retrigger[0]==2);
        c77_retrigger_report_acked(&e,type,old,0,now);assert(e.retrigger[0]==2);
        completed_phase(type);assert(e.retrigger[0]==3);
        completed_phase(type);assert(!e.retrigger[0]&&(e.pressed_mask&1));
        retrigger_travel(300);assert(!(e.pressed_mask&1));
    }
    /* Other input keeps reporting during this key's release; stale ACK cannot
     * skip the release phase or its explicit re-press completion. */
    retrigger_setup(binding[0]);retrigger_event();
    e.config.bindings[1]=(c77_binding){.kind=1,.code=5};e.pressed_mask|=2;
    uint8_t old_pressed[32],report[32];uint16_t consumer;
    c77_report(&e,old_pressed,&consumer);uint32_t held_tag=e.retrigger_epoch;
    completed_phase(0);c77_report(&e,report,&consumer);
    assert(!usage_on(report,4)&&usage_on(report,5));
    c77_retrigger_report_acked(&e,0,old_pressed,held_tag,now);assert(e.retrigger[0]==2);
    completed_phase(0);assert(e.retrigger[0]==3);
    c77_retrigger_report_acked(&e,0,old_pressed,held_tag,now);assert(e.retrigger[0]==3);
    completed_phase(0);assert(e.retrigger[0]==0);
    /* Duplicate usages never receive a synthetic release from another source. */
    retrigger_setup(binding[0]);retrigger_event();e.config.bindings[1]=binding[0];e.pressed_mask|=2;
    completed_phase(0);assert(!e.retrigger[0]);c77_report(&e,report,&consumer);assert(usage_on(report,4));
    /* Shared chord modifier stays down; unique chord usages still retrigger. */
    retrigger_setup(binding[1]);retrigger_event();e.config.bindings[1]=(c77_binding){.kind=1,.modifiers=2};e.pressed_mask|=2;
    completed_phase(0);c77_report(&e,report,&consumer);assert(report[28]==2&&!usage_on(report,4));
    completed_phase(0);assert(e.retrigger[0]==3);completed_phase(0);assert(!e.retrigger[0]);
    for(unsigned type=1;type<=2;type++){
        retrigger_setup(binding[type+1]);retrigger_event();e.config.bindings[1]=binding[type+1];e.pressed_mask|=2;
        completed_phase(type);assert(!e.retrigger[0]);
    }
    /* Relative bindings emit one extra delta, no fake physical release. */
    for(unsigned kind=2;kind<=3;kind++){
        retrigger_setup((c77_binding){.kind=(uint8_t)kind,.code=0xffff});retrigger_event();
        assert(!e.retrigger[0]&&(kind==2?e.wheel_pending:e.pan_pending)==-2);
        retrigger_travel(600);assert((kind==2?e.wheel_pending:e.pan_pending)==-2);
    }
    retrigger_setup((c77_binding){0});retrigger_event();assert(!e.retrigger[0]);
}
static void test_retrigger_cancellation(void){
    const c77_binding b={.kind=1,.code=4};uint8_t report[32];uint16_t consumer;
    for(unsigned phase=1;phase<=3;phase++){
        retrigger_setup(b);retrigger_event();
        for(unsigned p=1;p<phase;p++)completed_phase(0);
        uint32_t tag=e.retrigger_epoch;retrigger_travel(0);assert(!e.retrigger[0]&&!(e.pressed_mask&1));
        memset(report,0xff,sizeof report);c77_retrigger_report_acked(&e,0,report,tag,now);
        c77_report(&e,report,&consumer);assert(!usage_on(report,4)&&!e.retrigger[0]);
    }
    retrigger_setup(b);retrigger_event();unsigned drops=e.pulse_drops;
    retrigger_travel(325);retrigger_travel(475);assert(e.retrigger[0]==1&&e.pulse_drops==drops+1);
    /* A stalled host does not create an unbounded queue or eternal suppression. */
    e.retrigger_since[0]=now-100000; c77_tick(&e,now);assert(!e.retrigger[0]&&e.output);
    retrigger_setup(b);retrigger_event();c77_disable(&e);assert(!e.output&&!e.retrigger[0]);
    retrigger_setup(b);retrigger_event();uint8_t payload[22],blob[C77_CONFIG_BYTES],reply[64];
    c77_config_encode(&e.config,blob);memcpy(payload,blob,22);
    assert(command(4,payload,22,reply)==C77_OK&&!e.output&&!e.retrigger[0]&&e.config.keys[0].flags==15);
    retrigger_setup(b);retrigger_event();now+=6000;c77_tick(&e,now);assert(!e.output&&!e.retrigger[0]);
    /* Configuration roundtrip preserves the optional bit even when RT is off. */
    e.config.keys[0].flags=13;c77_config copy;c77_config_encode(&e.config,blob);
    assert(c77_config_decode(&copy,blob)&&copy.keys[0].flags==13);
    copy.keys[0].flags=16;assert(!c77_config_valid(&copy));
}
int main(void){
    test_retrigger_machine();test_retrigger_output();test_retrigger_cancellation();
    puts("retrigger: reversal, polarity, deadzones, every binding, duplicate union, stale ACK, cancellation and bounded pending PASS");
    return 0;
}
