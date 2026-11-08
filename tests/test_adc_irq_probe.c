/* Exercise the production sampled timing protocol with a fake DWT. */
#include "adc_irq_probe.h"
#include "adc_scan_private.h"
#include "keyboard.h"
#include "update_link.h"
#include "usb_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

volatile uint32_t adc_irq_probe_test_cycles;
static uint32_t sequence;
static usb_keyboard_stats_t usb;
static bool vendor_idle;
void usb_keyboard_stats(usb_keyboard_stats_t *out) {*out=usb;}
bool usb_vendor_idle(void) {return vendor_idle;}
void adc_scan_stats(split_adc_snapshot *out) {memset(out,0,sizeof *out);out->sample_seq=sequence;}
static uint32_t word(const uint8_t *p)
{
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint8_t request[64]={'C','7',1,ADC_IRQ_PROBE_COMMAND,1,0,0,1,0};
static uint8_t reply[64];
static void command(uint8_t action,uint8_t page,bool allow)
{
    request[7]=(uint8_t)(action+1U);request[8]=action;request[9]=page;
    update_frame_finish(request);
    assert(adc_irq_probe_command(request,reply,allow));
    assert(update_frame_valid(reply)&&reply[3]==(ADC_IRQ_PROBE_COMMAND|0x80U));
}
int main(void)
{
    assert(!adc_irq_probe_select(0));
    command(1,0,false);assert(reply[6]==C77_NOT_READY);
    command(0,0,false);assert(reply[6]==C77_BUSY);
    adc_irq_probe_test_cycles=UINT32_MAX-30U;sequence=UINT32_MAX-125U;
    command(0,0,true);assert(reply[6]==0U&&reply[7]==4U&&word(reply+8)==1U);
    adc_irq_probe_poll(true);assert(!adc_irq_probe_active&&adc_irq_probe_busy());
    vendor_idle=true;adc_irq_probe_poll(true);assert(adc_irq_probe_active);
    assert(!adc_irq_probe_select(adc_irq_probe_next-1U));
    const uint32_t origin=adc_irq_probe_test_cycles,seq_origin=sequence;
    uint32_t samples=0U,phases=0U,last_seq=0U;
    for(unsigned i=0;i<32000U;i++){
        const uint32_t start=origin+i*3750U;
        if(adc_irq_probe_select(sequence)){
            if(samples)assert(sequence-last_seq>=96U&&sequence-last_seq<=159U);
            last_seq=sequence;phases|=1U<<(sequence&7U);
            adc_irq_probe_test_cycles=start+20U;adc_irq_probe_mark(1);
            adc_irq_probe_test_cycles=start+60U;adc_irq_probe_mark(2);
            adc_irq_probe_test_cycles=start+120U;adc_irq_probe_mark(3);
            adc_irq_probe_test_cycles=start+200U;adc_irq_probe_mark(4);
            adc_irq_probe_finish(start,sequence+1U,start+250U);
            samples++;
        }
        sequence++;
    }
    assert(samples>200U&&samples<310U&&phases==255U);
    assert(adc_irq_probe_select(adc_irq_probe_next));
    adc_irq_probe_finish(0,adc_irq_probe_next,50); /* No publication or marks. */
    adc_irq_probe_test_cycles=origin+119999999U;
    adc_irq_probe_poll(true);assert(adc_irq_probe_busy());
    adc_irq_probe_test_cycles=origin+120000100U;usb.published=8000;usb.completed=4000;
    adc_irq_probe_poll(true);assert(!adc_irq_probe_active&&!adc_irq_probe_busy());
    command(1,0,false);
    assert(reply[6]==0U&&reply[7]==40U&&word(reply+8)==1U);
    assert(word(reply+12)==120000100U&&word(reply+16)==240000000U&&word(reply+20)==120000000U);
    assert(word(reply+24)==samples&&word(reply+28)==1U);
    assert(word(reply+32)==8000U&&word(reply+36)==4000U&&word(reply+40)==sequence-seq_origin);
    assert(word(reply+44)==0U);
    const uint32_t expected[]={250U,20U,40U,60U,80U,50U};
    for(unsigned page=1;page<=2;page++){
        command(1,(uint8_t)page,false);assert(reply[6]==0U&&reply[7]==44U&&reply[12]==3U);
        for(unsigned j=0;j<3;j++){
            const unsigned id=(page-1U)*3U+j;
            const uint8_t *p=reply+13+j*13U;
            assert(p[0]==id&&word(p+1)==samples&&word(p+5)==samples*expected[id]&&word(p+9)==expected[id]);
        }
    }
    command(1,3,false);assert(reply[6]==C77_BAD_INDEX);
    command(0,0,true);assert(word(reply+8)==2U);
    vendor_idle=false;adc_irq_probe_test_cycles+=12000000U;
    adc_irq_probe_poll(true);assert(!adc_irq_probe_busy());
    command(1,0,false);assert(reply[6]==C77_NOT_READY);
    puts("C86 ADC IRQ probe: randomized sparse phases, complete frames, wrap, exact partition and paging PASS");
    return 0;
}
