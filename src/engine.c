#include "keyboard.h"
#include "hardware_profile.h"
#include <string.h>
static void install(c77_engine *e,unsigned i){
    const c77_key_config *k=&e->config.keys[i];
    rt_config_t c={.adc_released=k->released,.adc_pressed=k->pressed,.actuation=k->actuation,.rt_press=k->rt_press,.rt_release=k->rt_release,.top_deadzone=k->top_deadzone,.bottom_deadzone=k->bottom_deadzone,.curve_id=k->curve_id,.enabled=(k->flags&5)==5,.rapid_trigger_enabled=(k->flags&2)!=0};
    (void)rt_key_configure(&e->keys[i],&c);
}
void c77_engine_init(c77_engine *e,const c77_config *cfg,uint32_t generation,bool stored){
    memset(e,0,sizeof *e);e->config=*cfg;e->generation=generation;e->stored_valid=(uint8_t)stored;e->cal_key=255;
    for(unsigned i=0;i<C77_HALL;i++)install(e,i);
}
void c77_disable(c77_engine *e){
    e->output=0;e->pressed_mask=0;e->wheel_pending=e->pan_pending=0;
    memset(e->pending,0,sizeof e->pending);memset(e->pulse,0,sizeof e->pulse);
    for(unsigned i=0;i<C77_HALL;i++)rt_key_reset(&e->keys[i]);
}
#if SCAN_ADC_ASYNC
#define C86_LOCAL_BANK_MAX_AGE_US 1000U
#endif
bool c77_source_fresh(const c77_engine *e,unsigned side,uint32_t now){
#if SCAN_ADC_ASYNC
    if(side==0U)return e->local_banks_valid==3U&&
        (uint32_t)(now-e->sample[0].timestamp_us)<=C86_LOCAL_BANK_MAX_AGE_US;
#endif
    return side<2&&e->seen[side]&&e->sample[side].valid&&(uint32_t)(now-e->sample[side].timestamp_us)<=5000u;
}
bool c77_enable_output(c77_engine *e,uint32_t now){
    if(e->cal_state||!c77_source_fresh(e,0,now)||!c77_source_fresh(e,1,now))return false;
    for(unsigned i=0;i<C77_HALL;i++)
        if((e->config.keys[i].flags&1)&&!(e->config.keys[i].flags&4))return false;
    c77_disable(e);
    e->restore_output_pending=false;
    e->output=1;
    return true;
}
static void add_scroll(c77_engine *e,int32_t *value,int delta){
    int32_t next=*value+delta;if(next>10000){next=10000;e->pulse_drops++;}if(next< -10000){next=-10000;e->pulse_drops++;}*value=next;
}
static void encoder_events(c77_engine *e,unsigned encoder,uint16_t count,bool first){
    int delta=(int16_t)(uint16_t)(count-e->encoder_previous[encoder]);e->encoder_previous[encoder]=count;
    if(first||!e->output||!delta)return;
    /* Impossible jumps after reset/noise never create thousands of HID events. */
    if(delta>32||delta< -32){e->errors++;return;}
    unsigned index=encoder*2+(delta<0?1u:0u);if(delta<0)delta=-delta;
    const c77_binding *b=&e->config.bindings[20+index];
    if(b->kind==2||b->kind==3){add_scroll(e,b->kind==2?&e->wheel_pending:&e->pan_pending,delta*(int16_t)b->code);return;}
    if(!b->kind)return;
    unsigned pending=e->pending[index]+(unsigned)delta;
    if(pending>16){e->pulse_drops+=pending-16;pending=16;}e->pending[index]=(uint8_t)pending;
}
static void calibrate_sample(c77_engine *e,uint16_t raw){
    if(e->cal_state!=1&&e->cal_state!=3)return;
    if(!e->cal_count){e->cal_min=e->cal_max=raw;e->cal_sum=0;}
    if(raw<e->cal_min)e->cal_min=raw;
    if(raw>e->cal_max)e->cal_max=raw;
    e->cal_sum+=raw;e->cal_count++;
    if(e->cal_count<256)return;
    if(e->cal_max-e->cal_min>64){e->cal_state=0;e->cal_key=255;e->errors++;return;}
    uint16_t average=(uint16_t)((e->cal_sum+128)/256);
    if(e->cal_state==1){e->cal_rest=average;e->cal_state=2;}else{e->cal_pressed=average;e->cal_state=4;}
}
static inline bool update_hall(c77_engine *e,unsigned side,const c77_sample_t *s,
                               bool first,unsigned start,unsigned end){
    bool valid=true;
    for(unsigned j=start;j<end;j++){
        unsigned i=side*7+j;uint16_t raw=s->raw[j];if(raw>4095){valid=false;e->errors++;continue;}
        if(e->cal_key==i)calibrate_sample(e,raw);
        rt_result_t result;
#if SCAN_DUAL_96K
        if(side==0U && s->hall_q5_valid) {
            uint32_t q5=s->hall_q5[j];
            if(q5>RT_ADC_Q5_MAX){valid=false;e->errors++;continue;}
            uint32_t old=e->filtered_q5[j],delta=q5>old?q5-old:old-q5;
            if(first || !e->filtered_q5_ready[j] ||
               delta>=(uint32_t)e->config.keys[i].noise*RT_ADC_Q5_SCALE)
                e->filtered_q5[j]=q5;
            e->filtered_q5_ready[j]=1U;
            result=rt_key_process_q5(&e->keys[i],e->filtered_q5[j]);
        } else
#endif
        {
        uint16_t decision_raw=raw;
#if HARDWARE_REVISION==86
        /* Only the local left acquisition path publishes this private field.
         * Right S7 frames and raw calibration/STATUS remain unchanged. */
        if(side==0U && s->hall_filtered_valid) {
            decision_raw=s->hall_filtered[j];
            if(decision_raw>4095U){valid=false;e->errors++;continue;}
        }
#endif
        int delta=(int)decision_raw-e->filtered[i];if(delta<0)delta=-delta;
#if SCAN_DUAL_96K
        bool changed_path=side==0U && e->filtered_q5_ready[j];
        if(first||changed_path||delta>=e->config.keys[i].noise)e->filtered[i]=decision_raw;
        if(side==0U)e->filtered_q5_ready[j]=0U;
#else
        if(first||delta>=e->config.keys[i].noise)e->filtered[i]=decision_raw;
#endif
        result=rt_key_process(&e->keys[i],e->filtered[i]);
        }
        e->travel[i]=result.travel;
        if(result.pressed)e->pressed_mask|=1u<<i;else e->pressed_mask&=~(1u<<i);
    }
    return valid;
}
static inline void update_digital(c77_engine *e,unsigned side,const c77_sample_t *s,uint32_t now,bool first){
    if(first){memset(e->digital_state+side*3,0,3);memset(e->digital_candidate+side*3,0,3);}
    for(unsigned j=0;j<3;j++){
        unsigned i=side*3+j;uint8_t state=(s->digital>>j)&1u;
        if(first||state!=e->digital_candidate[i]){e->digital_candidate[i]=state;e->digital_since[i]=now;}
        if((uint32_t)(now-e->digital_since[i])>=3000u)e->digital_state[i]=state;
        if(e->digital_state[i])e->pressed_mask|=1u<<(14+i);else e->pressed_mask&=~(1u<<(14+i));
    }
}
static inline void scroll_press_edges(c77_engine *e,uint32_t previous_pressed,bool first){
    /* A Hall/MX scroll binding emits one tick on each new press, never a
     * scan-rate-dependent repeat. RT re-presses are ordinary press edges. */
    if(e->output&&!first){
        uint32_t edges=e->pressed_mask&~previous_pressed;
        for(unsigned i=0;i<20;i++)if(edges&(1u<<i)){
            const c77_binding *b=&e->config.bindings[i];
            if(b->kind==2||b->kind==3)add_scroll(e,b->kind==2?&e->wheel_pending:&e->pan_pending,(int16_t)b->code);
        }
    }
}
void c77_update(c77_engine *e,unsigned side,const c77_sample_t *s,uint32_t now){
    if(side>1||!s||!s->valid)return;
    if((uint32_t)(now-s->timestamp_us)>5000u)return;
    if(e->seen[side]&&s->sequence==e->seen_sequence[side])return;
    bool first=!e->seen[side]||(uint32_t)(now-e->sample[side].timestamp_us)>5000u||(int32_t)(s->sequence-e->seen_sequence[side])<0;
    if(first&&e->seen[side]){
        c77_disable(e);
        if((unsigned)e->cal_key/7u==side){e->cal_state=0;e->cal_key=255;}
    }
    uint32_t previous_pressed=e->pressed_mask;
    e->seen[side]=1;e->seen_sequence[side]=s->sequence;e->sample[side]=*s;
#if SCAN_DUAL_96K
    if(side==0U && s->hall_q5_valid>1U) {
        e->sample[side].valid=0;e->errors++;return;
    }
#endif
    e->sample[side].valid=(uint8_t)update_hall(e,side,s,first,0U,7U);
    update_digital(e,side,s,now,first);
    scroll_press_edges(e,previous_pressed,first);
    encoder_events(e,side*2,s->encoders[0],first);encoder_events(e,side*2+1,s->encoders[1],first);
}
#if SCAN_ADC_ASYNC
bool c77_update_left_bank(c77_engine *e,unsigned bank,const c77_sample_t *s,uint32_t now){
    if(bank>=2U||!s||!s->valid||!s->hall_filtered_valid||
       (uint32_t)(now-s->timestamp_us)>C86_LOCAL_BANK_MAX_AGE_US)return false;
    const uint8_t bit=(uint8_t)(1U<<bank);
    const bool seen=(e->local_banks_seen&bit)!=0U;
    const uint32_t previous_sequence=e->local_bank_sequence[bank];
    if(seen&&s->sequence==previous_sequence)return false;
    const bool first=!(e->local_banks_valid&bit)||
        (uint32_t)(now-e->local_bank_time[bank])>C86_LOCAL_BANK_MAX_AGE_US||
        (int32_t)(s->sequence-previous_sequence)<0;
    const unsigned start=bank?SCAN_ADC1_RANKS:0U;
    const unsigned end=bank?SCAN_ADC_RANKS:SCAN_ADC1_RANKS;
    if(first&&seen){
        c77_disable(e);
        if(e->cal_key>=start&&e->cal_key<end){e->cal_state=0;e->cal_key=255;}
    }

    const uint32_t previous_pressed=e->pressed_mask;
    c77_sample_t *combined=&e->sample[0];
    e->local_bank_sequence[bank]=s->sequence;
    e->local_bank_time[bank]=s->timestamp_us;
    e->local_banks_seen|=bit;
    e->seen[0]=1U;
    for(unsigned key=start;key<end;key++){
        combined->raw[key]=s->raw[key];
        combined->hall_filtered[key]=s->hall_filtered[key];
    }
    if(update_hall(e,0U,s,first,start,end))e->local_banks_valid|=bit;
    else e->local_banks_valid&=(uint8_t)~bit;
    if(bank==0U){
        combined->digital=s->digital;
        combined->encoders[0]=s->encoders[0];combined->encoders[1]=s->encoders[1];
        update_digital(e,0U,s,now,first);
    }
    scroll_press_edges(e,previous_pressed,first);
    if(bank==0U){
        encoder_events(e,0U,s->encoders[0],first);
        encoder_events(e,1U,s->encoders[1],first);
    }
    /* STATUS keeps its whole-side wire format. Use the older bank timestamp
     * so that new data in one bank cannot refresh the other bank. */
    const unsigned other=bank^1U;
    const uint32_t other_time=e->local_bank_time[other];
    combined->timestamp_us=(e->local_banks_seen&(1U<<other))&&
        (uint32_t)(now-other_time)>(uint32_t)(now-s->timestamp_us)?other_time:s->timestamp_us;
    if(s->errors>combined->errors)combined->errors=s->errors;
    combined->valid=(uint8_t)(e->local_banks_valid==3U);
    combined->hall_filtered_valid=combined->valid;
    combined->sequence++;
    return true;
}
#endif
void c77_tick(c77_engine *e,uint32_t now){
    uint32_t elapsed=(uint32_t)(now-e->uptime_anchor_us)/1000u;
    e->uptime_ms+=elapsed;e->uptime_anchor_us+=elapsed*1000u;
#if SCAN_ADC_ASYNC
    for(unsigned bank=0;bank<2U;bank++)
        if((uint32_t)(now-e->local_bank_time[bank])>C86_LOCAL_BANK_MAX_AGE_US)
            e->local_banks_valid&=(uint8_t)~(1U<<bank);
    e->sample[0].valid=(uint8_t)(e->local_banks_valid==3U);
    e->sample[0].hall_filtered_valid=e->sample[0].valid;
#endif
    for(unsigned side=0;side<2;side++)if(!c77_source_fresh(e,side,now)){
        e->pressed_mask&=~((0x7fu<<(side*7))|(7u<<(14+side*3)));
        if(e->output)c77_disable(e);
    }
    if((e->cal_state==1||e->cal_state==3)&&(uint32_t)(now-e->cal_started)>5000000u){e->cal_state=0;e->cal_key=255;e->errors++;}
    if(!e->output)return;
    for(unsigned i=0;i<8;i++){
        if(!e->pulse[i]&&e->pending[i]){e->pending[i]--;e->pulse[i]=1;}
        if(e->pulse[i]==2&&(uint32_t)(now-e->pulse_started[i])>=2000u)e->pulse[i]=3;
    }
}
static inline bool is_keyboard_binding(const c77_binding *binding){
#if HARDWARE_REVISION == 86
    return binding->kind==1||binding->kind==C77_BINDING_KEY_CHORD;
#else
    return binding->kind==1;
#endif
}
static inline void add_usage(uint8_t keyboard[32],uint8_t usage){
    if(usage)keyboard[usage/8u]|=(uint8_t)(1u<<(usage%8u));
}
static inline bool has_usage(const uint8_t keyboard[32],uint8_t usage){
    return usage&&(keyboard[usage/8u]&(1u<<(usage%8u)));
}
static void bind_report(const c77_binding *b,uint8_t keyboard[32],uint16_t *consumer){
    if(is_keyboard_binding(b)){
        add_usage(keyboard,(uint8_t)b->code);
        keyboard[0xe0/8]|=b->modifiers;
#if HARDWARE_REVISION == 86
        if(b->kind==C77_BINDING_KEY_CHORD){
            add_usage(keyboard,(uint8_t)(b->code>>8));
            add_usage(keyboard,b->third_key);
        }
#endif
    }else if(b->kind==4&&!*consumer)*consumer=b->code;
}
void c77_report(const c77_engine *e,uint8_t keyboard[32],uint16_t *consumer){
    memset(keyboard,0,32);*consumer=0;if(!e->output)return;
    /* Encoder consumer pulses have priority over held consumer buttons. */
    for(unsigned i=0;i<8;i++)if(e->pulse[i]==1||e->pulse[i]==2)bind_report(&e->config.bindings[20+i],keyboard,consumer);
    for(unsigned i=0;i<20;i++)if(e->pressed_mask&(1u<<i))bind_report(&e->config.bindings[i],keyboard,consumer);
}
void c77_keyboard_report_acked(c77_engine *e,const uint8_t actual[32],uint32_t now){
    if(!actual)return;
    for(unsigned i=0;i<8;i++)if((e->pulse[i]==1||e->pulse[i]==3)&&is_keyboard_binding(&e->config.bindings[20+i])){
        const c77_binding *b=&e->config.bindings[20+i];
        const uint8_t first=(uint8_t)b->code,mods=actual[0xe0/8]&b->modifiers;
        const bool first_present=has_usage(actual,first);
        bool all_present=(!first||first_present)&&mods==b->modifiers;
        bool all_absent=!first_present&&!mods;
#if HARDWARE_REVISION == 86
        if(b->kind==C77_BINDING_KEY_CHORD){
            const bool second_present=has_usage(actual,(uint8_t)(b->code>>8));
            const bool third_present=has_usage(actual,b->third_key);
            all_present=all_present&&second_present&&(!b->third_key||third_present);
            all_absent=all_absent&&!second_present&&!third_present;
        }
#endif
        /* The endpoint may just have completed an older mailbox snapshot.
         * Never acknowledge a new pulse merely because some report completed.
         * A held duplicate usage/modifier conservatively delays release ACK. */
        if(e->pulse[i]==1&&all_present){e->pulse[i]=2;e->pulse_started[i]=now;}
        else if(e->pulse[i]==3&&all_absent)e->pulse[i]=0;
    }
}
void c77_keyboard_acked(c77_engine *e,uint32_t now){
    uint8_t actual[32];uint16_t consumer;c77_report(e,actual,&consumer);
    c77_keyboard_report_acked(e,actual,now);
}
void c77_consumer_acked(c77_engine *e,uint32_t now){
    /* Only one consumer usage per report; don't consume unsent simultaneous pulses. */
    uint8_t keyboard[32];uint16_t selected;c77_report(e,keyboard,&selected);
    for(unsigned i=0;i<8;i++)if(e->config.bindings[20+i].kind==4){
        if(e->pulse[i]==1&&e->config.bindings[20+i].code==selected){e->pulse[i]=2;e->pulse_started[i]=now;break;}
        if(e->pulse[i]==3&&selected!=e->config.bindings[20+i].code)e->pulse[i]=0;
    }
}
void c77_mouse_peek(const c77_engine *e,int8_t *wheel,int8_t *pan){
    int32_t w=e->output?e->wheel_pending:0,p=e->output?e->pan_pending:0;
    *wheel=(int8_t)(w>127?127:w< -127?-127:w);*pan=(int8_t)(p>127?127:p< -127?-127:p);
}
uint8_t c77_mouse_buttons(const c77_engine *e){
    uint8_t buttons=0;if(!e->output)return 0;
    for(unsigned i=0;i<28;i++){
        const c77_binding *b=&e->config.bindings[i];
        bool active=i<20?(e->pressed_mask&(1u<<i))!=0:(e->pulse[i-20]==1||e->pulse[i-20]==2);
        if(active&&b->kind==5)buttons|=(uint8_t)(1u<<(b->code-1));
    }return buttons;
}
void c77_mouse_accepted(c77_engine *e,int8_t wheel,int8_t pan){e->wheel_pending-=wheel;e->pan_pending-=pan;}
void c77_mouse_acked(c77_engine *e,uint8_t buttons,uint32_t now){
    for(unsigned i=0;i<8;i++)if(e->config.bindings[20+i].kind==5){
        uint8_t bit=(uint8_t)(1u<<(e->config.bindings[20+i].code-1));
        if(e->pulse[i]==1&&(buttons&bit)){e->pulse[i]=2;e->pulse_started[i]=now;}
        else if(e->pulse[i]==3&&!(buttons&bit))e->pulse[i]=0;
    }
}
/* Reinstall after mutation without losing diagnostics. */
void c77_reconfigure(c77_engine *e){c77_disable(e);e->restore_output_pending=false;for(unsigned i=0;i<C77_HALL;i++)install(e,i);e->dirty=1;}
