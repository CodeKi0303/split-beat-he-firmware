#include "fw_runtime.h"
#include "fw_update.h"
#include "platform.h"
#include "update_link.h"
#include "scan_profile.h"
#if SCAN_ADC_PHASE_PAIR && !ROLE_LEFT && !FW_UPDATE_BOOT
#include "adc_async.h"
#endif
#include <string.h>
#if ROLE_LEFT
#include "usb_device.h"
#endif

/* Exactly one request/reply in flight. Main keeps servicing CC while a right
 * flash erase runs; no ADC or key output is promised in maintenance mode. */
static uint8_t mode,reply[64],query[64];
static bool ready,ack_wait,maintenance,relay,relay_sent,relay_power,resume_after_ack;
static uint32_t started,reply_at;
#if ROLE_LEFT
static uint32_t usb_generation;
#endif
static void error_reply(const uint8_t q[64],uint8_t status)
{
    memset(reply,0,64);reply[0]='C';reply[1]='7';reply[2]=1;
    reply[3]=q[3]|0x80U;reply[4]=q[4];reply[5]=q[5];reply[6]=status;
    update_frame_finish(reply);ready=true;reply_at=board_micros();
}
static void pause(void)
{
    if(!maintenance){board_update_pause();maintenance=true;}
}
void fw_runtime_init(uint8_t selected_mode)
{
    mode=selected_mode;ready=ack_wait=relay=relay_sent=relay_power=resume_after_ack=false;
    maintenance=mode==FW_UPDATE_MODE_BOOT;
#if ROLE_LEFT
    usb_generation=usb_session_generation();
#endif
}
bool fw_runtime_busy(void){return ready||ack_wait||relay;}
bool fw_runtime_maintenance(void){return maintenance;}
bool fw_runtime_relay_power(void){return relay_power;}
bool fw_runtime_request(const uint8_t q[64])
{
#if HARDWARE_REVISION!=86
    (void)q;return false;
#else
    if(!q||q[0]!='C'||q[1]!='7')return false;
#if SCAN_ADC_PHASE_PAIR && !ROLE_LEFT && !FW_UPDATE_BOOT
    if(q[3]==ADC_PAIR_COMMAND && mode==FW_UPDATE_MODE_APP && !maintenance && !fw_runtime_busy()){
        uint32_t hz;
        uint8_t filter;
        if(!update_frame_valid(q)||q[6]||q[7]!=5U)error_reply(q,FW_UPDATE_BAD_PACKET);
        else if(!adc_scan_parse_tuning(q+8,&hz,&filter))error_reply(q,FW_UPDATE_BAD_CONFIG);
        else if(!adc_scan_tune(hz,filter)||adc_scan_rate()!=hz||adc_scan_filter()!=filter)error_reply(q,FW_UPDATE_NOT_READY);
        else{
            error_reply(q,0U);
            reply[7]=5U;memcpy(reply+8,q+8,5U);update_frame_finish(reply);
        }
        return true;
    }
#endif
    bool local=q[3]>=FW_UPDATE_INFO&&q[3]<=FW_UPDATE_READ;
#if ROLE_LEFT
    bool remote=q[3]>=0x50U&&q[3]<=0x56U;
#else
    bool remote=false;
#endif
    if(!local&&!remote)return false;
    if(fw_runtime_busy())return false;
    if(!update_frame_valid(q)||q[6]){error_reply(q,FW_UPDATE_BAD_PACKET);return true;}
    if(remote){
        static const uint8_t sizes[]={0,4,32,40,4,4,5};
        if(q[7]!=sizes[q[3]-0x50U]){error_reply(q,FW_UPDATE_BAD_PACKET);return true;}
        /* Inspecting the satellite pauses normal input too. The updater UI
         * exposes an explicit exit; never silently re-enable HID output. */
        pause();relay_power=true;memcpy(query,q,64);query[3]-=0x20U;
        update_frame_finish(query);relay=true;relay_sent=false;started=board_micros();return true;
    }
#if ROLE_LEFT && FW_UPDATE_BOOT
    if(q[3]>=FW_UPDATE_BEGIN&&q[3]<=FW_UPDATE_COMMIT){
        /* The left CPU stalls during its own single-bank flash write. Never
         * keep powering an unserviced downstream port during that stall. */
        board_update_power_off();relay_power=false;
    }
#endif
    if(!fw_update_handle(q,reply,mode))return false;
    ready=true;reply_at=board_micros();
    if(!reply[6]&&q[3]==FW_UPDATE_ENTER)pause();
    if(!reply[6]&&q[3]==FW_UPDATE_RUN&&mode==FW_UPDATE_MODE_APP)
        resume_after_ack=true;
    return true;
#endif
}
static void reply_completed(void)
{
    ack_wait=false;
    unsigned action=fw_update_take_action();
    if(action==FW_UPDATE_ACTION_ENTER||action==FW_UPDATE_ACTION_RUN){
        fw_boot_reset(action==FW_UPDATE_ACTION_ENTER);return;
    }
    if(resume_after_ack){
        resume_after_ack=false;
        if(maintenance){board_update_resume();maintenance=false;}
        relay_power=false;
    }
}
void fw_runtime_poll(void)
{
#if HARDWARE_REVISION==86
    /* Deadline wins even if the first poll after expiry sees idle/complete. */
    if((ready||ack_wait)&&(uint32_t)(board_micros()-reply_at)>3000000U){
        ready=ack_wait=false;fw_update_cancel_action();resume_after_ack=false;
    }
#if ROLE_LEFT
    uint32_t generation=usb_session_generation();
    if(generation!=usb_generation){
        usb_generation=generation;ready=ack_wait=relay=relay_sent=resume_after_ack=false;
        fw_update_cancel_action();
        /* Keep maintenance latched. A right bootloader may already have
         * accepted ENTER. Reconnection may inspect/retry, never auto-run. */
    }
    if(relay){
        uint8_t incoming[64];
        if(!relay_sent)relay_sent=split_update_send(query);
        if(relay_sent&&split_update_read(incoming)&&update_frame_valid(incoming)&&
           incoming[3]==(uint8_t)(query[3]|0x80U)&&incoming[4]==query[4]&&incoming[5]==query[5]){
            memcpy(reply,incoming,64);reply[3]+=0x20U;update_frame_finish(reply);
            relay=false;ready=true;reply_at=board_micros();
        }
        uint32_t limit=query[3]==FW_UPDATE_BEGIN?14000000U:2500000U;
        if(relay&&(uint32_t)(board_micros()-started)>limit){
            uint8_t original[64];memcpy(original,query,64);original[3]+=0x20U;
            error_reply(original,FW_UPDATE_NOT_READY);relay=false;
        }
    }
    if(ack_wait&&usb_vendor_idle())reply_completed();
    if(ready&&usb_vendor_send(reply)){ready=false;ack_wait=true;}
#else
    if(ack_wait&&split_update_tx_idle()){
        if(split_update_tx_succeeded())reply_completed();
        else{ack_wait=false;fw_update_cancel_action();resume_after_ack=false;}
    }
    if(ready&&split_update_send(reply)){ready=false;ack_wait=true;}
    if(!fw_runtime_busy()){
        uint8_t packet[64];
        if(split_update_read(packet)) (void)fw_runtime_request(packet);
    }
#endif
#endif
}
