#include "hardware_profile.h"
#if HARDWARE_REVISION==86 && ROLE_LEFT
#include "performance.h"
#include "update_link.h"
#include "usb_device.h"
#include "adc_scan_private.h"
#include "platform.h"
#include "split_link_diag.h"
#include <string.h>
extern volatile uint32_t diagnostic_cc_state,diagnostic_cc_error;
static uint32_t main_loops,main_loop_gap_max_us,last_loop_us;
static uint32_t source_updates[2],source_sequence[2];
static bool have_loop,have_source[2];
void performance_loop_begin(uint32_t now_us)
{
    if(have_loop) {
        const uint32_t gap=now_us-last_loop_us;
        if(gap>main_loop_gap_max_us)main_loop_gap_max_us=gap;
    }
    last_loop_us=now_us;have_loop=true;main_loops++;
}
void performance_sample_update(unsigned side,uint32_t sequence)
{
    if(side>=2U||sequence==0U)return;
    if(!have_source[side]||sequence!=source_sequence[side]) {
        source_sequence[side]=sequence;have_source[side]=true;source_updates[side]++;
    }
}
static void header(const uint8_t q[64],uint8_t r[64],uint8_t status)
{
    memset(r,0,64);r[0]='C';r[1]='7';r[2]=1;r[3]=q[3]|0x80U;
    r[4]=q[4];r[5]=q[5];r[6]=status;
}
bool performance_busy_reply(const uint8_t q[64],uint8_t r[64])
{
    if(!update_frame_valid(q)||q[6]||(q[3]&0x80U))return false;
    header(q,r,5);update_frame_finish(r);return true;
}
bool performance_command(const uint8_t q[64],uint8_t r[64])
{
    if(q[0]!='C'||q[1]!='7'||(q[3]!=0x40U&&q[3]!=0x41U&&q[3]!=0x42U&&q[3]!=0x43U))return false;
    header(q,r,0);
    if(!update_frame_valid(q)||q[6]||q[7])r[6]=2;
    else{
        uint32_t values[13];
        if(q[3]==0x40U){
            usb_keyboard_stats_t usb;split_adc_snapshot adc;
            usb_keyboard_stats(&usb);adc_scan_stats(&adc);
            uint32_t local[13]={board_micros(),usb.completed,usb.published,usb.submit_failures,
                usb.stale_neutralizations,adc.sample_seq,adc.errors,adc.overruns,adc.late_irqs,
                adc.period_min_cycles,adc.period_max_cycles,adc.irq_max_cycles,SCAN_TARGET_HZ};
            memcpy(values,local,sizeof values);
        }else if(q[3]==0x41U){
            split_link_stats_t link;split_link_stats(&link);
            uint32_t local[13]={board_micros(),link.polls,link.accepted,link.timeouts,
                link.sequence_errors,link.late_replies,link.uart_errors,link.tx_busy,
                link.tx_fail,link.crc_errors,link.framing_errors,link.rtt_min_us,link.rtt_max_us};
            memcpy(values,local,sizeof values);
        }else if(q[3]==0x42U){
            split_adc_snapshot adc;adc_scan_stats(&adc);
            uint32_t local[13]={adc.running,adc.sample_seq,adc.errors,adc.overruns,
                adc.dma_errors,adc.late_irqs,adc.init_error,adc.stop_reason,
                adc.stop_phase_cycles,adc.stop_dma_flags,adc.stop_ndtr1,
                adc.stop_ndtr2,SCAN_TARGET_HZ};
            memcpy(values,local,sizeof values);
        }else{
            split_link_stats_t link;split_link_stats(&link);
            uint32_t local[13]={board_micros(),main_loops,main_loop_gap_max_us,
                source_updates[0],source_updates[1],diagnostic_cc_state,diagnostic_cc_error,
                link.flags,link.rx_bytes,link.uart_irq_max_cycles,link.poll_irq_max_cycles,
                link.uart_irqs,link.poll_irqs};
            memcpy(values,local,sizeof values);
        }
        for(unsigned i=0;i<13U;i++)for(unsigned j=0;j<4U;j++)r[8+i*4U+j]=(uint8_t)(values[i]>>(8U*j));
        r[7]=52;
    }
    update_frame_finish(r);return true;
}
#endif
