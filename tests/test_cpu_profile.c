/* Drive the production timer and packet state machine with a fake DWT. */
#include "cpu_profile.h"
#include "keyboard.h"
#include "update_link.h"
#include "usb_device.h"
#include <assert.h>
#include <stdio.h>

volatile uint32_t cpu_prof_test_cycles;
static usb_keyboard_stats_t usb;
static bool vendor_idle;
void usb_keyboard_stats(usb_keyboard_stats_t *out) {*out=usb;}
bool usb_vendor_idle(void) {return vendor_idle;}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint8_t request[64]={'C','7',1,CPU_PROFILE_COMMAND,1,0,0,1,0};
static uint8_t reply[64];
static void command(uint8_t action,uint8_t page,bool allow)
{
    request[7]=(uint8_t)(action+1U);request[8]=action;request[9]=page;
    update_frame_finish(request);
    assert(cpu_profile_command(request,reply,allow));
    assert(update_frame_valid(reply)&&reply[3]==(CPU_PROFILE_COMMAND|0x80U));
}
static void row(unsigned id,uint32_t calls,uint32_t total,uint32_t max)
{
    command(1,(uint8_t)(1U+id/3U),false);
    assert(reply[6]==0U && reply[7]==44U && word(reply+8)==1U && reply[12]==3U);
    const uint8_t *p=reply+13+id%3U*13U;
    assert(p[0]==id&&word(p+1)==calls&&word(p+5)==total&&word(p+9)==max);
}
int main(void)
{
    command(1,0,false);assert(reply[6]==C77_NOT_READY);
    command(0,0,false);assert(reply[6]==C77_BUSY&&!cpu_profile_busy());
    cpu_prof_test_cycles=UINT32_MAX-300U;
    command(0,0,true);assert(reply[6]==0U&&reply[7]==4U&&word(reply+8)==1U);
    cpu_profile_poll(false);assert(!cpu_prof_active&&cpu_profile_busy());
    cpu_profile_poll(true);assert(!cpu_prof_active);
    command(1,0,true);assert(reply[6]==C77_BUSY);
    vendor_idle=true;usb.published=UINT32_MAX-10U;usb.completed=42U;
    cpu_profile_poll(true);assert(cpu_prof_active&&cpu_profile_busy());
    const uint32_t start=cpu_prof_test_cycles;
    cpu_prof_test_cycles=start+20U;CPU_PROFILE_BEGIN(main);
    cpu_prof_test_cycles=start+30U;CPU_PROFILE_BEGIN(lower_irq);
    cpu_prof_test_cycles=start+40U;CPU_PROFILE_BEGIN(adc_irq);
    cpu_prof_test_cycles=start+50U;CPU_PROFILE_IRQ_END(adc_irq,CPU_PROF_IRQ_ADC);
    cpu_prof_test_cycles=start+65U;CPU_PROFILE_IRQ_END(lower_irq,CPU_PROF_IRQ_SPLIT);
    cpu_prof_test_cycles=start+100U;CPU_PROFILE_END(main,CPU_PROF_SERVICE);
    cpu_prof_test_cycles=start+101U;CPU_PROFILE_RT_CALL();CPU_PROFILE_BEGIN(curve);
    cpu_prof_test_cycles=start+300U;CPU_PROFILE_END(curve,CPU_PROF_CURVE_MISS);
    CPU_PROFILE_RT_CALL();
    cpu_prof_test_cycles=start+59999999U;
    cpu_profile_poll(true);assert(cpu_prof_active);
    usb.published+=2500U;usb.completed+=2000U;
    cpu_prof_test_cycles=start+60000042U;
    cpu_profile_poll(true);assert(!cpu_prof_active&&!cpu_profile_busy());
    command(1,0,false);
    assert(reply[6]==0U&&reply[7]==32U&&word(reply+8)==1U);
    assert(word(reply+12)==60000042U&&word(reply+16)==240000000U&&word(reply+20)==60000000U);
    assert(word(reply+24)==2500U&&word(reply+28)==2000U&&word(reply+32)==35U&&word(reply+36)==1U);
    row(CPU_PROF_SERVICE,1,45,45);
    row(CPU_PROF_IRQ_ADC,1,10,10);
    row(CPU_PROF_IRQ_SPLIT,1,25,25);
    row(CPU_PROF_CURVE_MISS,1,199,199);
    row(CPU_PROF_RT_CALLS,2,0,0);
    command(1,5,false);assert(reply[6]==C77_BAD_INDEX);
    command(0,0,true);assert(word(reply+8)==2U&&cpu_profile_busy());
    vendor_idle=false;cpu_prof_test_cycles+=12000000U;
    cpu_profile_poll(true);assert(!cpu_profile_busy()&&!cpu_prof_active);
    command(1,0,false);assert(reply[6]==C77_NOT_READY);
    command(0,0,true);vendor_idle=true;cpu_profile_poll(true);
    assert(cpu_prof_active);
    cpu_profile_cancel();assert(!cpu_prof_active&&!cpu_profile_busy());
    command(1,0,false);assert(reply[6]==C77_NOT_READY);
    puts("C86 CPU profile: nested timing, counter wrap, paging, drain/timeout and cancel PASS");
    return 0;
}
