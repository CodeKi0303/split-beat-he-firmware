/* Reported endpoint clamp versus frozen v0.3.40 decisions. No device access. */
#include "keyboard.h"
#include "hardware_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Byte-identical v0.3.40 RT source at e8b2557df26437de1aaad66df5a4af0a7c251003.
 * Compile its public entry points under distinct names for a real old/new
 * state-machine comparison, not a second hand-written expectation model. */
#define rt_config_defaults previous_config_defaults
#define rt_key_reset previous_key_reset
#define rt_key_configure previous_key_configure
#define rt_key_process previous_key_process
#define rt_key_process_q5 previous_key_process_q5
#include "fixtures/rapid_trigger_v040.c"
#undef rt_config_defaults
#undef rt_key_reset
#undef rt_key_configure
#undef rt_key_process
#undef rt_key_process_q5

static unsigned comparisons,edges,repeats;
static uint16_t expected_report(uint16_t travel,const rt_config_t *c)
{
    if(travel<=c->top_deadzone)return 0U;
    if(travel>=1000U-c->bottom_deadzone)return 1000U;
    return travel;
}
static void compare_step(rt_key_t *current,rt_key_t *old,uint32_t input,bool q5)
{
    const rt_result_t a=q5?rt_key_process_q5(current,input):rt_key_process(current,(uint16_t)input);
    const rt_result_t b=q5?previous_key_process_q5(old,input):previous_key_process(old,(uint16_t)input);
    assert(a.pressed==b.pressed&&a.changed==b.changed&&a.retriggered==b.retriggered);
    assert(current->travel==old->travel&&current->extreme==old->extreme);
    assert(current->pressed==old->pressed&&current->rapid_cycle==old->rapid_cycle);
    assert(current->valid==old->valid&&current->retrigger_armed==old->retrigger_armed);
    assert(current->retrigger_valley==old->retrigger_valley);
    assert(current->normalization_kind==old->normalization_kind);
    assert(current->normalization_input==old->normalization_input);
    assert(current->normalization_travel==old->normalization_travel);
    assert(a.travel==expected_report(b.travel,&current->config));
    comparisons++;edges+=a.changed;repeats+=a.retriggered;
}
static void test_boundaries(void)
{
    const uint16_t travel[]={0,19,20,21,200,400,899,900,901,999,1000};
    const uint16_t report[]={0,0,0,21,200,400,899,1000,1000,1000,1000};
    for(unsigned reverse=0;reverse<2U;reverse++)for(unsigned q5=0;q5<2U;q5++)
    for(unsigned mode=0;mode<4U;mode++){
        rt_config_t c;rt_config_defaults(&c,reverse?2000U:1000U,reverse?1000U:2000U);
        c.bottom_deadzone=100U;c.enabled=(mode&1U)!=0U;c.rapid_trigger_enabled=(mode&2U)!=0U;
        rt_key_t k;assert(rt_key_configure(&k,&c));
        for(unsigned i=0;i<sizeof travel/sizeof travel[0];i++){
            const uint16_t raw=(uint16_t)(reverse?2000U-travel[i]:1000U+travel[i]);
            rt_result_t r=q5?rt_key_process_q5(&k,raw*32U):rt_key_process(&k,raw);
            assert(k.travel==travel[i]&&r.travel==report[i]);
            if(!c.enabled)assert(!r.pressed);
        }
        c.top_deadzone=0U;c.bottom_deadzone=0U;assert(rt_key_configure(&k,&c));
        for(unsigned t=0;t<=1000U;t++){
            const uint16_t raw=(uint16_t)(reverse?2000U-t:1000U+t);
            rt_result_t r=q5?rt_key_process_q5(&k,raw*32U):rt_key_process(&k,raw);
            assert(r.travel==t&&k.travel==t);
        }
    }
}
static void test_previous_decision_parity(void)
{
    const uint16_t zones[][2]={{0U,0U},{20U,100U},{150U,400U}};
    const uint16_t motion[]={0,20,200,600,550,600,700,550,650,1000,950,900,850,851,400,100,0};
    uint32_t random=0x53825340U;
    for(unsigned curve=0;curve<2U;curve++)for(unsigned reverse=0;reverse<2U;reverse++)
    for(unsigned mode=0;mode<8U;mode++)for(unsigned zone=0;zone<3U;zone++){
        rt_config_t c;rt_config_defaults(&c,reverse?3000U:1000U,reverse?1000U:3000U);
        c.curve_id=(uint16_t)curve;c.enabled=(mode&1U)!=0U;
        c.rapid_trigger_enabled=(mode&2U)!=0U;c.retrigger_before_release=(mode&4U)!=0U;
        c.top_deadzone=zones[zone][0];c.bottom_deadzone=zones[zone][1];
        c.rt_press=50U;c.rt_release=200U;
        rt_key_t current,old;assert(rt_key_configure(&current,&c)&&previous_key_configure(&old,&c));
        for(unsigned i=0;i<sizeof motion/sizeof motion[0];i++){
            const unsigned raw=reverse?3000U-2U*motion[i]:1000U+2U*motion[i];
            compare_step(&current,&old,raw,false);
        }
        /* Whole/Q5 ramp and reversal include every physical ADC code. */
        for(unsigned raw=0;raw<4096U;raw++)compare_step(&current,&old,raw,false);
        for(int raw=4095;raw>=0;raw--)compare_step(&current,&old,(uint32_t)raw*32U,true);
        /* Fractional input, cache hits and API switching retain old decisions. */
        for(unsigned i=0;i<2048U;i++){
            random=random*1664525U+1013904223U;
            const uint32_t input=random%(RT_ADC_Q5_MAX+1U);
            compare_step(&current,&old,input,true);
            compare_step(&current,&old,input,true);
            compare_step(&current,&old,input/32U,false);
        }
    }
    assert(comparisons>1000000U&&edges>1000U&&repeats>0U);
}
static void status_packet(c77_engine *e,unsigned side,uint32_t now,uint8_t response[64])
{
    uint8_t request[64]={0};request[0]='C';request[1]='7';request[2]=1U;
    request[3]=2U;request[4]=1U;request[7]=1U;request[8]=(uint8_t)side;
    const uint16_t crc=c77_crc16(request,62);request[62]=(uint8_t)crc;request[63]=(uint8_t)(crc>>8);
    assert(c77_command(e,request,response,now,2U,1024U)&&response[6]==0U);
}
static unsigned read16(const uint8_t *p){return p[0]|((unsigned)p[1]<<8);}
static void test_engine_status(void)
{
    c77_config c;c77_defaults(&c);
    for(unsigned i=0;i<C77_HALL;i++){
        c.keys[i].released=1000U;c.keys[i].pressed=2000U;c.keys[i].flags=7U;
        c.keys[i].top_deadzone=20U;c.keys[i].bottom_deadzone=100U;c.keys[i].noise=0U;
    }
    c77_engine e;c77_engine_init(&e,&c,0U,false);
    const uint16_t position[]={19U,20U,21U,899U,900U,901U};
    const uint16_t reported[]={0U,0U,21U,899U,1000U,1000U};
    for(unsigned n=0;n<6U;n++)for(unsigned side=0;side<2U;side++){
        c77_sample_t s={0};s.valid=1U;s.sequence=n+1U;s.timestamp_us=1000U+n*20U;
        for(unsigned k=0;k<7U;k++){
            s.raw[k]=(uint16_t)(1000U+position[n]);
            s.hall_filtered[k]=s.raw[k];
#if SCAN_DUAL_96K
            s.hall_q5[k]=(uint32_t)s.raw[k]*32U;
#endif
        }
        if(side==0U){
            /* Local decisions use conditioned input; STATUS raw is not clamped. */
            s.raw[0]=2222U;s.hall_filtered_valid=1U;
#if SCAN_DUAL_96K
            s.hall_q5_valid=1U;
#endif
        }
        c77_update(&e,side,&s,s.timestamp_us);
        uint8_t response[64];status_packet(&e,side,s.timestamp_us,response);
        assert(read16(response+8U+16U)==(side?1000U+position[n]:2222U));
        assert(read16(response+8U+30U)==reported[n]);
        assert(e.keys[side*7U].travel==position[n]&&e.travel[side*7U]==reported[n]);
        assert(!e.output);
    }
}
int main(void)
{
    test_boundaries();test_previous_decision_parity();test_engine_status();
    printf("Deadzone export: inclusive boundaries, zero zones, polarity/Q5, engine STATUS, %u old/new decisions (%u edges, %u retriggers) PASS\n",comparisons,edges,repeats);
    return 0;
}
