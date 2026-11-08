/* Actual runtime/RS485 codec with transport/core mocks; never touches hardware. */
#include "fw_runtime.h"
#include "fw_update.h"
#include "update_link.h"
#include "platform.h"
#include "usb_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_MODE (FW_UPDATE_BOOT?FW_UPDATE_MODE_BOOT:FW_UPDATE_MODE_APP)
static uint32_t now,generation;
static unsigned pauses,resumes,power_offs,resets,cancels,core_calls,usb_sends,split_sends;
static unsigned pending_action,power_offs_at_core;
static bool allow_usb,allow_split,usb_idle,split_idle,split_success,last_reset_enter;
static bool input_queued,core_no_action;
static uint8_t input[64],usb_out[64],split_out[64],core_status;
uint32_t board_micros(void){return now;}
void board_update_pause(void){pauses++;}
void board_update_resume(void){resumes++;}
void board_update_power_off(void){power_offs++;}
void fw_boot_reset(bool enter){resets++;last_reset_enter=enter;}
uint32_t usb_session_generation(void){return generation;}
bool usb_vendor_idle(void){return usb_idle;}
bool usb_vendor_send(const uint8_t packet[64]){
    if(!allow_usb||!usb_idle)return false;
    /* USB can return BAD_PACKET echoing an invalid transaction 0. */
    assert(update_frame_valid(packet)||(!packet[4]&&!packet[5]&&packet[6]==FW_UPDATE_BAD_PACKET));
    memcpy(usb_out,packet,64);usb_sends++;usb_idle=false;return true;
}
bool split_update_send(const uint8_t packet[64]){
    if(!allow_split||!split_idle||!update_frame_valid(packet))return false;
    assert(update_frame_valid(packet));memcpy(split_out,packet,64);split_sends++;split_idle=false;split_success=false;return true;
}
bool split_update_read(uint8_t packet[64]){
    if(!input_queued)return false;
    memcpy(packet,input,64);input_queued=false;return true;
}
bool split_update_tx_idle(void){return split_idle;}
bool split_update_tx_succeeded(void){return split_success;}
unsigned fw_update_take_action(void){unsigned result=pending_action;pending_action=0;return result;}
void fw_update_cancel_action(void){pending_action=0;cancels++;}
bool fw_update_handle(const uint8_t q[64],uint8_t r[64],uint8_t mode){
    static const uint8_t sizes[]={0,4,32,40,4,4,5};
    core_calls++;power_offs_at_core=power_offs;
    if(q[3]<FW_UPDATE_INFO||q[3]>FW_UPDATE_READ)return false;
    memset(r,0,64);memcpy(r,q,6);r[3]|=0x80U;r[6]=core_status;
    if(q[7]!=sizes[q[3]-FW_UPDATE_INFO])r[6]=FW_UPDATE_BAD_PACKET;
    if((q[3]==FW_UPDATE_ENTER||q[3]==FW_UPDATE_RUN)&&memcmp(q+8,"UPD3",4))r[6]=FW_UPDATE_BAD_CONFIG;
    if(!r[6]&&!core_no_action){
        if(q[3]==FW_UPDATE_ENTER&&mode==FW_UPDATE_MODE_APP)pending_action=FW_UPDATE_ACTION_ENTER;
        if(q[3]==FW_UPDATE_RUN&&mode==FW_UPDATE_MODE_BOOT)pending_action=FW_UPDATE_ACTION_RUN;
    }
    update_frame_finish(r);return true;
}
static void request(uint8_t q[64],uint8_t op){
    static const uint8_t sizes[]={0,4,32,40,4,4,5};
    memset(q,0,64);q[0]='C';q[1]='7';q[2]=1;q[3]=op;q[4]=0x37;q[5]=0x12;
    uint8_t local=op>=0x50U?(uint8_t)(op-0x20U):op;
    if(local>=FW_UPDATE_INFO&&local<=FW_UPDATE_READ)q[7]=sizes[local-FW_UPDATE_INFO];
    if(local==FW_UPDATE_ENTER||local==FW_UPDATE_RUN)memcpy(q+8,"UPD3",4);
    update_frame_finish(q);
}
static void response(uint8_t r[64],const uint8_t q[64],uint8_t status){
    memset(r,0,64);memcpy(r,q,6);r[3]|=0x80U;r[6]=status;update_frame_finish(r);
}
static void inject_frame(const uint8_t frame[64]){
    update_frame_parser parser={0};bool received=false;
    for(unsigned i=0;i<64;i++)received|=update_frame_feed(&parser,frame[i],input);
    assert(received);input_queued=true;
}
static void setup(void){
    now=100;generation=7;pauses=resumes=power_offs=resets=cancels=core_calls=usb_sends=split_sends=0;
    pending_action=power_offs_at_core=0;core_status=0;core_no_action=false;
    allow_usb=allow_split=usb_idle=split_idle=true;split_success=false;last_reset_enter=false;input_queued=false;
    memset(usb_out,0,64);memset(split_out,0,64);fw_runtime_init(TEST_MODE);
    assert(!fw_runtime_busy());assert(fw_runtime_maintenance()==(TEST_MODE==FW_UPDATE_MODE_BOOT));
    assert(!fw_runtime_relay_power());
}
static const uint8_t *sent_reply(void){
#if ROLE_LEFT
    return usb_out;
#else
    return split_out;
#endif
}
static unsigned sent_count(void){
#if ROLE_LEFT
    return usb_sends;
#else
    return split_sends;
#endif
}
static void complete_reply(bool success){
#if ROLE_LEFT
    (void)success;usb_idle=true;
#else
    split_idle=true;split_success=success;
#endif
}
static void local_ack(void){
    setup();uint8_t q[64];request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);
    assert(fw_runtime_request(q)&&fw_runtime_busy());assert(!resets);
    fw_runtime_poll();assert(sent_count()==1&&fw_runtime_busy()&&!resets);
    assert(!fw_runtime_request(q));fw_runtime_poll();assert(!resets);
    complete_reply(true);fw_runtime_poll();assert(resets==1);
    assert(last_reset_enter==!FW_UPDATE_BOOT);assert(!fw_runtime_busy()&&!resumes);
    setup();request(q,FW_UPDATE_INFO);assert(fw_runtime_request(q));
    assert(!fw_runtime_request(q)&&core_calls==1);fw_runtime_poll();complete_reply(true);fw_runtime_poll();
    assert(!resets&&!resumes&&!fw_runtime_busy());
    setup();request(q,FW_UPDATE_ENTER);core_status=FW_UPDATE_NOT_READY;
    assert(fw_runtime_request(q));fw_runtime_poll();complete_reply(true);fw_runtime_poll();
    assert(!resets&&!pauses&&!resumes);
}
static void strict_requests(void){
    for(unsigned kind=0;kind<7;kind++){
        setup();uint8_t q[64];request(q,FW_UPDATE_INFO);
        if(kind==0)q[62]^=1;
        if(kind==1){q[2]=2;update_frame_finish(q);}
        if(kind==2){q[4]=q[5]=0;update_frame_finish(q);}
        if(kind==3){q[6]=1;update_frame_finish(q);}
        if(kind==4){q[7]=55;update_frame_finish(q);}
        if(kind==5){q[61]=1;update_frame_finish(q);}
        if(kind==6){q[7]=1;update_frame_finish(q);}
        assert(fw_runtime_request(q));fw_runtime_poll();
#if !ROLE_LEFT
        if(kind==2){assert(!sent_count()&&!core_calls);now+=3000001U;fw_runtime_poll();assert(!fw_runtime_busy());continue;}
#endif
        assert(sent_reply()[6]==FW_UPDATE_BAD_PACKET&&!pauses&&!resumes&&!resets&&!power_offs);
        if(kind!=6)assert(!core_calls);
    }
    setup();uint8_t q[64];request(q,0x20);assert(!fw_runtime_request(q));
    request(q,FW_UPDATE_INFO);q[0]='X';assert(!fw_runtime_request(q));assert(!fw_runtime_request(NULL));
    assert(!core_calls&&!fw_runtime_busy());
}
static void no_ack_no_reset(void){
    setup();uint8_t q[64];request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);
#if ROLE_LEFT
    allow_usb=false;
#else
    allow_split=false;
#endif
    assert(fw_runtime_request(q));fw_runtime_poll();assert(!sent_count()&&!resets&&fw_runtime_busy());
#if ROLE_LEFT
    allow_usb=true;
#else
    allow_split=true;
#endif
    fw_runtime_poll();assert(sent_count()==1);now+=3000001U;fw_runtime_poll();
    assert(!resets&&!resumes&&cancels==1&&!pending_action&&!fw_runtime_busy());
    complete_reply(true);fw_runtime_poll();assert(!resets&&!resumes);
    /* A completion first observed after the deadline must not win a race
     * against cancellation merely because endpoint idle is tested first. */
    setup();request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);assert(fw_runtime_request(q));
    fw_runtime_poll();now+=3000001U;complete_reply(true);fw_runtime_poll();
    assert(!resets&&!resumes&&cancels==1&&!pending_action);
    /* Deadline subtraction must also work across the microsecond rollover. */
    setup();now=UINT32_MAX-100U;request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);
    assert(fw_runtime_request(q));fw_runtime_poll();now+=3000001U;fw_runtime_poll();assert(!resets&&cancels==1);
    /* Unqueued replies expire too; an endpoint becoming free much later is
     * not permission to perform a forgotten reset or resume. */
    setup();request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);
#if ROLE_LEFT
    allow_usb=false;
#else
    allow_split=false;
#endif
    assert(fw_runtime_request(q));fw_runtime_poll();now+=3000001U;fw_runtime_poll();
    assert(!sent_count()&&!resets&&!resumes&&!fw_runtime_busy()&&cancels==1);
#if ROLE_LEFT
    allow_usb=true;
#else
    allow_split=true;
#endif
    fw_runtime_poll();assert(!sent_count()&&!resets&&!resumes);
}
static void explicit_resume(void){
    setup();uint8_t q[64];core_no_action=true;request(q,FW_UPDATE_ENTER);
    assert(fw_runtime_request(q));fw_runtime_poll();complete_reply(true);fw_runtime_poll();
    assert(fw_runtime_maintenance()&&!resumes&&!resets);
    request(q,FW_UPDATE_RUN);assert(fw_runtime_request(q));fw_runtime_poll();
    assert(fw_runtime_maintenance()&&!resumes);complete_reply(true);fw_runtime_poll();
    if(FW_UPDATE_BOOT)assert(fw_runtime_maintenance()&&!resumes);
    else assert(!fw_runtime_maintenance()&&resumes==1);
    assert(!resets);
}
static void power_guard(void){
    for(uint8_t op=FW_UPDATE_BEGIN;op<=FW_UPDATE_COMMIT;op++){
        setup();uint8_t q[64];request(q,op);assert(fw_runtime_request(q));
#if ROLE_LEFT && FW_UPDATE_BOOT
        assert(power_offs==1&&power_offs_at_core==1&&!fw_runtime_relay_power());
#else
        assert(!power_offs&&!power_offs_at_core);
#endif
    }
}
#if ROLE_LEFT
static void disconnect_cancels(void){
    setup();uint8_t q[64];request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);
    assert(fw_runtime_request(q));fw_runtime_poll();assert(fw_runtime_busy());
    generation++;usb_idle=true;fw_runtime_poll();
    assert(!resets&&!resumes&&!fw_runtime_busy()&&cancels==1&&!pending_action);
    assert(fw_runtime_maintenance());
    setup();request(q,0x50);assert(fw_runtime_request(q));fw_runtime_poll();
    assert(fw_runtime_maintenance()&&fw_runtime_relay_power());
    generation++;fw_runtime_poll();assert(!fw_runtime_busy()&&fw_runtime_maintenance()&&!resumes);
}
static void relay_validation(void){
    setup();uint8_t q[64],r[64];request(q,0x50);
    assert(fw_runtime_request(q)&&fw_runtime_busy()&&fw_runtime_maintenance()&&fw_runtime_relay_power());
    assert(pauses==(FW_UPDATE_BOOT?0U:1U));fw_runtime_poll();
    assert(split_sends==1&&split_out[3]==FW_UPDATE_INFO&&update_frame_valid(split_out)&&!core_calls&&!usb_sends);
    response(r,split_out,FW_UPDATE_OK);r[4]^=1;update_frame_finish(r);inject_frame(r);fw_runtime_poll();assert(fw_runtime_busy()&&!usb_sends);
    response(r,split_out,FW_UPDATE_OK);r[3]=FW_UPDATE_WRITE|0x80U;update_frame_finish(r);inject_frame(r);fw_runtime_poll();assert(fw_runtime_busy()&&!usb_sends);
    response(r,split_out,FW_UPDATE_OK);r[7]=3;r[8]=4;r[9]=5;r[10]=6;update_frame_finish(r);inject_frame(r);fw_runtime_poll();
    assert(usb_sends==1&&usb_out[3]==0xd0U&&usb_out[4]==q[4]&&usb_out[5]==q[5]);
    assert(usb_out[7]==3&&!memcmp(usb_out+8,r+8,3));complete_reply(true);fw_runtime_poll();
    assert(!fw_runtime_busy()&&fw_runtime_maintenance()&&fw_runtime_relay_power()&&!resumes&&!resets);
    /* Malformed remote request lengths are rejected before pausing input. */
    setup();request(q,0x50);q[7]=1;update_frame_finish(q);assert(fw_runtime_request(q));fw_runtime_poll();
    assert(usb_out[6]==FW_UPDATE_BAD_PACKET&&!split_sends&&!pauses&&!fw_runtime_relay_power());
    /* Even if a transport regression yields an unchecked frame, never forward
     * a bad-CRC or nonzero-padding response as a newly checksummed success. */
    setup();request(q,0x50);assert(fw_runtime_request(q));fw_runtime_poll();
    response(input,split_out,FW_UPDATE_OK);input[62]^=1;input_queued=true;fw_runtime_poll();
    assert(!usb_sends&&fw_runtime_busy());
}
static void relay_timeout(void){
    for(unsigned is_begin=0;is_begin<2U;is_begin++){
        setup();uint8_t q[64];request(q,is_begin?0x52:0x50);allow_split=false;
        assert(fw_runtime_request(q));fw_runtime_poll();assert(!split_sends&&!usb_sends);
        now+=2000000U;allow_split=true;fw_runtime_poll();assert(split_sends==1&&!usb_sends);
        now+=500001U;fw_runtime_poll();
        if(is_begin){assert(!usb_sends&&fw_runtime_busy());now+=11500000U;fw_runtime_poll();}
        assert(usb_sends==1&&usb_out[6]==FW_UPDATE_NOT_READY&&usb_out[3]==(uint8_t)(q[3]|0x80U));
        assert(!resets&&!resumes&&fw_runtime_maintenance()&&!power_offs);
    }
}
#else
static void right_transport(void){
    setup();uint8_t q[64];request(q,0x50);assert(!fw_runtime_request(q));
    request(q,FW_UPDATE_INFO);inject_frame(q);fw_runtime_poll();assert(core_calls==1&&!split_sends);
    fw_runtime_poll();assert(split_sends==1&&split_out[3]==(FW_UPDATE_INFO|0x80U));
    complete_reply(true);fw_runtime_poll();assert(!fw_runtime_busy()&&!resets);
    setup();request(q,FW_UPDATE_BOOT?FW_UPDATE_RUN:FW_UPDATE_ENTER);inject_frame(q);
    fw_runtime_poll();fw_runtime_poll();complete_reply(false);fw_runtime_poll();
    assert(!resets&&!resumes&&!pending_action&&cancels==1&&!fw_runtime_busy());
}
#endif
static void frame_codec(void){
    uint8_t q[64],out[64];request(q,FW_UPDATE_INFO);update_frame_parser parser={0};
    const uint8_t noise[]={0,0xff,'C','X',1,2,'C','7',2};
    for(unsigned i=0;i<sizeof noise;i++)assert(!update_frame_feed(&parser,noise[i],out));
    unsigned accepted=0;for(unsigned i=0;i<64;i++)accepted+=(unsigned)update_frame_feed(&parser,q[i],out);
    assert(accepted==1&&!memcmp(q,out,64)&&parser.errors);
    q[61]=1;update_frame_finish(q);assert(!update_frame_valid(q));
    q[61]=0;q[7]=55;update_frame_finish(q);assert(!update_frame_valid(q));
}
int main(void){
    frame_codec();local_ack();strict_requests();no_ack_no_reset();explicit_resume();power_guard();
#if ROLE_LEFT
    disconnect_cancels();relay_validation();relay_timeout();
#else
    right_transport();
#endif
    printf("fw_runtime role_left%d boot%d: ACK, cancellation, frame, relay and power guards PASS\n",ROLE_LEFT,FW_UPDATE_BOOT);
    return 0;
}
