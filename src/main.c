#include "stm32h7xx_hal.h"
#include "platform.h"
#include "keyboard.h"
#include "settings_store.h"
#include "flash_layout.h"
#include "hardware_profile.h"
#include "cpu_profile.h"
#include "adc_irq_probe.h"
#if SCAN_ADC_ASYNC
#include "adc_async.h"
#endif
#if SCAN_ADC_PHASE_PAIR && ROLE_LEFT
#include "update_link.h"
#include <string.h>
#endif
#if SCAN_ADC_CAPTURE_DIAG
#include "adc_trace.h"
#include "update_link.h"
#include <string.h>
#endif
#if HARDWARE_REVISION==86
#include "fw_runtime.h"
#include "fw_update.h"
#include "performance.h"
#endif
#if ROLE_LEFT
#include "usb_device.h"
#endif
volatile uint32_t diagnostic_fault_cfsr,diagnostic_fault_hfsr;
void _init(void){}
void _fini(void){}
void HardFault_Handler(void){diagnostic_fault_cfsr=SCB->CFSR;diagnostic_fault_hfsr=SCB->HFSR;Error_Handler();}
void MemManage_Handler(void){HardFault_Handler();}
void BusFault_Handler(void){HardFault_Handler();}
void UsageFault_Handler(void){HardFault_Handler();}
#if ROLE_LEFT
static c77_engine engine;
static uint8_t response[64],response_pending;
static uint32_t session,last_aux,last_command,save_accepted_at;
static uint8_t save_reply_queued;
#if SCAN_ADC_CAPTURE_DIAG
#include "adc_trace.inc"
#endif
#if SCAN_ADC_PHASE_PAIR
#include "adc_pair.inc"
#endif
#endif
int main(void){
    /* Set before HAL_Init enables SysTick. Pinned SystemInit leaves VTOR alone. */
    SCB->VTOR=H723_APP_ORIGIN;__DSB();__ISB();
    board_init();
    if(board_flash_kib()!=1024)Error_Handler();
#if HARDWARE_REVISION==86
    fw_update_init();
#endif
#if ROLE_LEFT
    c77_config config;uint32_t generation=0;c77_defaults(&config);
    bool saved_output=false;
    bool stored=settings_store_load(&config,&generation,&saved_output)!=0;
#if HARDWARE_REVISION==86
    if(fw_boot_output_hold_take())saved_output=false;
#endif
    c77_engine_init(&engine,&config,generation,stored);
    engine.restore_output_pending=stored&&saved_output;
    usb_device_init();session=usb_session_generation();
#endif
    board_start();
#if HARDWARE_REVISION==86
    fw_runtime_init(FW_UPDATE_MODE_APP);
#endif
    for(;;){
#if defined(C86_CPU_PROFILE)
        cpu_profile_poll(!response_pending);
#endif
#if defined(C86_ADC_IRQ_PROBE)
        adc_irq_probe_poll(!response_pending);
#endif
        CPU_PROFILE_BEGIN(prof_service);
#if HARDWARE_REVISION==86 && ROLE_LEFT
        performance_loop_begin(board_micros());
#endif
        board_service();
#if ROLE_LEFT
        uint32_t now=board_micros();uint32_t current_session=usb_session_generation();
        if(session!=current_session){
            session=current_session;c77_disable(&engine);engine.cal_state=0;engine.cal_key=255;
#if SCAN_ADC_PHASE_PAIR
            adc_pair_cancel();
#endif
            response_pending=0;engine.save_requested=0;save_reply_queued=0;
#if defined(C86_CPU_PROFILE)
            cpu_profile_cancel();
#endif
#if defined(C86_ADC_IRQ_PROBE)
            adc_irq_probe_cancel();
#endif
#if SCAN_ADC_CAPTURE_DIAG
            adc_trace.pending=false;
#endif
        }
        if(!usb_configured())c77_disable(&engine);
        usb_device_poll();
        /* A queued/published report is not an ACK. Only consume encoder
         * pulses actually represented in a completed USB IN report. */
        uint8_t completed[32];uint32_t completed_count;
        if(usb_keyboard_take_completed(completed,&completed_count))
            c77_keyboard_report_acked(&engine,completed,now);
#if HARDWARE_REVISION==86
        fw_runtime_poll();
#if SCAN_ADC_PHASE_PAIR
        adc_pair_poll(now);
#endif
        if(fw_runtime_maintenance()){
            c77_disable(&engine);engine.cal_state=0;engine.cal_key=255;
            engine.restore_output_pending=false;
        }
#endif
        CPU_PROFILE_END(prof_service,CPU_PROF_SERVICE);
        CPU_PROFILE_BEGIN(prof_left);
        c77_sample_t sample;
#if SCAN_ADC_ASYNC
        bool local_advanced=false;
        for(unsigned bank=0;bank<2U;bank++){
            if(adc_scan_bank_snapshot(bank,&sample)&&
               c77_update_left_bank(&engine,bank,&sample,board_micros()))
                local_advanced=true;
        }
        if(local_advanced)performance_sample_update(0,engine.sample[0].sequence);
#else
#if SCAN_ADC_CAPTURE_DIAG
        adc_trace_history history;
        const bool traced=adc_trace.pending &&
            adc_scan_trace_snapshot(&sample,&history,adc_trace.keys);
        if(traced || board_local_snapshot(&sample)) {
            const bool trace_new=traced && (!engine.seen[0] || engine.seen_sequence[0]!=sample.sequence);
#else
        if(board_local_snapshot(&sample)) {
#endif
            c77_update(&engine,0,&sample,board_micros());
#if SCAN_ADC_CAPTURE_DIAG
            if(trace_new && engine.seen_sequence[0]==sample.sequence && engine.sample[0].valid)
                trace_response(C77_OK,&sample,&history);
#endif
#if HARDWARE_REVISION==86
            performance_sample_update(0,sample.sequence);
#endif
        }
#if SCAN_ADC_CAPTURE_DIAG
        if(adc_trace.pending && (uint32_t)(board_micros()-adc_trace.requested_us)>=10000U)
            trace_response(C77_NOT_READY,NULL,NULL);
#endif
#endif
        CPU_PROFILE_END(prof_left,CPU_PROF_LEFT);
        CPU_PROFILE_BEGIN(prof_right);
        if(board_remote_snapshot(&sample)) {
            c77_update(&engine,1,&sample,board_micros());
#if HARDWARE_REVISION==86
            performance_sample_update(1,sample.sequence);
#endif
        }
        CPU_PROFILE_END(prof_right,CPU_PROF_RIGHT);
        CPU_PROFILE_BEGIN(prof_report);
        now=board_micros();
        c77_tick(&engine,now);
        if(engine.restore_output_pending&&usb_configured()
#if SCAN_ADC_PHASE_PAIR
           && !adc_pair.uncertain
#endif
        )
            (void)c77_enable_output(&engine,now);
        {
            uint8_t keys[32];uint16_t consumer;c77_report(&engine,keys,&consumer);
            /* Host IN tokens determine cadence. Publish only the newest
             * state; DataIn immediately re-arms without software125us drift. */
            (void)usb_keyboard_publish(keys);
        }
        if((uint32_t)(now-last_aux)>=1000u){
            uint8_t keys[32];uint16_t consumer;int8_t wheel,pan;c77_report(&engine,keys,&consumer);
            if(usb_consumer_send(consumer))c77_consumer_acked(&engine,now);
            c77_mouse_peek(&engine,&wheel,&pan);
            uint8_t buttons=c77_mouse_buttons(&engine);
            /* Send neutral button state too: disabling output, stale sources,
             * and a release must never leave a mouse button held on the host. */
            if(usb_mouse_send(buttons,wheel,pan)){
                c77_mouse_accepted(&engine,wheel,pan);c77_mouse_acked(&engine,buttons,now);
            }
            last_aux=now;
        }
        CPU_PROFILE_END(prof_report,CPU_PROF_REPORT);
        CPU_PROFILE_BEGIN(prof_vendor);
        if(response_pending&&usb_vendor_send(response)){
            response_pending=0;
            if(engine.save_requested){save_reply_queued=1;save_accepted_at=now;}
        }
        if(!response_pending&&!engine.save_requested&&(uint32_t)(now-last_command)>=1000u
#if SCAN_ADC_PHASE_PAIR
           && !adc_pair.pending
#endif
#if SCAN_ADC_CAPTURE_DIAG
           &&!adc_trace.pending
#endif
#if HARDWARE_REVISION==86
           &&!fw_runtime_busy()
#endif
        ){
            uint8_t request[64];if(usb_vendor_read(request)){
                last_command=now;
#if HARDWARE_REVISION==86
#if defined(C86_ADC_IRQ_PROBE)
                if(adc_irq_probe_command(request,response,!engine.output&&!engine.dirty&&!engine.cal_state&&
                        engine.stored_valid&&!fw_runtime_maintenance()&&usb_speed()==2U&&
                        c77_source_fresh(&engine,0,now)&&c77_source_fresh(&engine,1,now))){
                    response_pending=1;continue;
                }
                if(adc_irq_probe_busy()){
                    if(performance_busy_reply(request,response))response_pending=1;
                    continue;
                }
#endif
#if defined(C86_CPU_PROFILE)
                if(cpu_profile_command(request,response,!engine.output&&!engine.dirty&&!engine.cal_state&&
                        engine.stored_valid&&!fw_runtime_maintenance()&&usb_speed()==2U&&
                        c77_source_fresh(&engine,0,now)&&c77_source_fresh(&engine,1,now))){
                    response_pending=1;CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
                if(cpu_profile_busy()){
                    if(performance_busy_reply(request,response))response_pending=1;
                    CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
#endif
                if(fw_runtime_request(request)){
                    CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
                if(performance_command(request,response)){
                    response_pending=1;CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
                if(fw_runtime_maintenance()&&request[3]!=1&&request[3]!=2&&request[3]!=3&&request[3]!=9){
                    /* No settings save/reset/enable during an interrupted
                     * satellite update. Local UPDATE_RUN exits explicitly. */
                    if(performance_busy_reply(request,response))response_pending=1;
                    CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);
                    continue;
                }
#if SCAN_ADC_PHASE_PAIR
                if(adc_pair_request(request,now)||
                   (adc_pair.uncertain&&request[3]==6U&&request[7]==1U&&request[8]==1U&&
                    (response_pending=(uint8_t)performance_busy_reply(request,response)))){
                    CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
                if(adc_tuning_command(&engine,request,response,now)){
                    response_pending=1;CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
#endif
#if SCAN_ADC_CAPTURE_DIAG
                if(trace_request(request,now)){
                    CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);continue;
                }
#endif
#endif
                response_pending=(uint8_t)c77_command(&engine,request,response,now,usb_speed(),board_flash_kib());
            }
        }
        if(engine.save_requested&&save_reply_queued&&usb_vendor_idle()&&(uint32_t)(now-save_accepted_at)>=25000u){
            /* Single-bank flash stalls execution. No input/USB promises during save. */
            c77_disable(&engine);usb_device_stop();board_stop();
            (void)settings_store_save(&engine.config,&engine.generation,engine.save_output);
            board_reboot();
        }
        CPU_PROFILE_END(prof_vendor,CPU_PROF_VENDOR);
#else
        /* No RT on the satellite. Only normal link interrupts read its snapshot. */
#if HARDWARE_REVISION==86
        fw_runtime_poll();
#endif
#if SCAN_ADC_ASYNC
        /* The next DMA boundary can be later than the next S7 request. Keep
         * consuming new completed windows without waiting for a boundary IRQ. */
        if(!fw_runtime_maintenance())adc_scan_satellite_service();
        else __WFI();
#else
        __WFI();
#endif
#endif
    }
}
