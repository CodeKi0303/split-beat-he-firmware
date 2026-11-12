/* Real descriptors and HID callbacks, fake hardware transport. This tests
 * protocol/state/lifetime invariants, not physical HS timing or enumeration. */
#include <assert.h>
#include <stdio.h>
#include "usb_stubs/usbd_conf.h"
#include "../src/usb_device.c"
#include "../src/usb_descriptors.c"

uint32_t fake_basepri, fake_ipsr, fake_tick;
static uint8_t sent[4][64], control[128];
static uint32_t sends[4], rx_size=64, arms, stalls, ctl_size, flushes;
static uint8_t *control_rx;
static USBD_StatusTypeDef transmit_result=USBD_OK;
USBD_StatusTypeDef USBD_Init(USBD_HandleTypeDef *d,USBD_DescriptorsTypeDef *desc,uint8_t id)
{ memset(d,0,sizeof *d); d->pDesc=desc; d->id=id; return USBD_OK; }
USBD_StatusTypeDef USBD_RegisterClass(USBD_HandleTypeDef *d,USBD_ClassTypeDef *c)
{ d->pClass[0]=c; return USBD_OK; }
USBD_StatusTypeDef USBD_Start(USBD_HandleTypeDef *d)
{ d->dev_state=USBD_STATE_CONFIGURED; return (USBD_StatusTypeDef)d->pClass[0]->Init(d,1); }
USBD_StatusTypeDef USBD_Stop(USBD_HandleTypeDef *d)
{ d->dev_state=USBD_STATE_DEFAULT; return USBD_OK; }
USBD_StatusTypeDef USBD_DeInit(USBD_HandleTypeDef *d)
{ return (USBD_StatusTypeDef)d->pClass[0]->DeInit(d,1); }
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *d,uint8_t ep,uint8_t type,uint16_t size)
{ (void)d; assert(type==3); assert((ep==0x81 && size==32) || (ep==0x82&&size==5) || (ep==0x83&&size==2) || ((ep==0x84||ep==4)&&size==64)); return USBD_OK; }
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *d,uint8_t ep)
{ (void)d; (void)ep; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *d,uint8_t ep)
{ (void)d; (void)ep; ++flushes; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *p,uint32_t n)
{ (void)d; assert(ep==4 && n==64 && p==rx_irq); ++arms; return USBD_OK; }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *d,uint8_t ep)
{ (void)d; (void)ep; return rx_size; }
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *d,uint8_t ep,uint8_t *p,uint32_t n)
{
    (void)d; assert(ep>=0x81 && ep<=0x84 && n<=64);
    if(ep==0x82) assert(n==5); /* Includes neutral, idle and ordinary mouse sends. */
    /* Only EP1 may rearm from its DataIn callback. Main owns all other paths
     * and masks USB while publishing/copying the HAL-owned transfer storage. */
    assert((fake_ipsr==0 && fake_basepri!=0) || (fake_ipsr!=0 && ep==0x81));
    if(transmit_result!=USBD_OK) return transmit_result;
    memcpy(sent[ep-0x81],p,n); ++sends[ep-0x81]; return USBD_OK;
}
USBD_StatusTypeDef USBD_CtlSendData(USBD_HandleTypeDef *d,uint8_t *p,uint32_t n)
{ (void)d; assert(n<=sizeof control); memcpy(control,p,n); ctl_size=n; return USBD_OK; }
USBD_StatusTypeDef USBD_CtlPrepareRx(USBD_HandleTypeDef *d,uint8_t *p,uint32_t n)
{ (void)d; assert(n==64); control_rx=p; return USBD_OK; }
void USBD_CtlError(USBD_HandleTypeDef *d,USBD_SetupReqTypedef *r)
{ (void)d; (void)r; ++stalls; }
void USBD_GetString(uint8_t *s,uint8_t *dst,uint16_t *n)
{ *n=(uint16_t)(2+2*strlen((char *)s)); dst[0]=(uint8_t)*n; dst[1]=3; for(unsigned i=0;s[i];++i) {dst[2+2*i]=s[i];dst[3+2*i]=0;} }
static uint8_t request(uint8_t bm,uint8_t op,uint16_t value,uint16_t iface,uint16_t n)
{ USBD_SetupReqTypedef r={bm,op,value,iface,n}; return c77_hid_class.Setup(&c77_usb_device,&r); }
static void complete_all(void)
{ for(uint8_t i=1;i<=4;++i) (void)c77_hid_class.DataIn(&c77_usb_device,i); }
static void complete_keyboard_irq(void)
{
    assert(fake_ipsr==0);fake_ipsr=30;
    assert(c77_hid_class.DataIn(&c77_usb_device,1)==USBD_OK);
    fake_ipsr=0;
}
static void desc_check(uint8_t *d,size_t n,uint8_t type,uint8_t vendor_interval)
{
    assert(n==116 && d[1]==type && (d[2]+256U*d[3])==n && d[4]==4 && d[8]==250);
    unsigned interfaces=0,endpoints=0;
    for(size_t off=0;off<n;) {
        unsigned len=d[off]; assert(len>=2 && off+len<=n);
        if(d[off+1]==4) {assert(d[off+2]==interfaces++ && d[off+5]==3 && d[off+6]==0);}
        if(d[off+1]==5) { ++endpoints; uint8_t ep=d[off+2];
            assert(d[off+3]==3); assert(d[off+6]==((ep==4||ep==0x84)?vendor_interval:1));
            if(ep==0x82) assert(d[off+4]==5 && d[off+5]==0); }
        off+=len;
    }
    assert(interfaces==4 && endpoints==5);
}
static void bits_check(const uint8_t *d,size_t n,unsigned input,unsigned output)
{
    unsigned size=0,report_count=0,in=0,out=0;
    for(size_t off=0;off<n;) {
        unsigned tag=d[off++],len=tag&3U,val=0; if(len==3) len=4;
        assert(off+len<=n); for(unsigned j=0;j<len;++j) val|=(unsigned)d[off++]<<(8*j);
        if((tag&0xFC)==0x74)size=val;
        if((tag&0xFC)==0x94)report_count=val;
        if((tag&0xFC)==0x80)in+=size*report_count;
        if((tag&0xFC)==0x90)out+=size*report_count;
        assert((tag&0xFC)!=0x84); /* No report ID byte. */
    }
    assert(in==input && out==output);
}
static void mouse_axes_check(void)
{
    /* Parse actual HID local usages and main fields, not byte-pattern matching.
     * A Mouse collection without X/Y can parse yet fail Windows mouhid startup. */
    unsigned page=0,size=0,field_count=0,bit=0,usage[8]={0},used=0,found=0;
    for(size_t off=0;off<sizeof mouse_report_desc;) {
        unsigned tag=mouse_report_desc[off++],len=tag&3U,value=0;
        if(len==3)len=4;
        assert(off+len<=sizeof mouse_report_desc);
        for(unsigned j=0;j<len;++j)value|=(unsigned)mouse_report_desc[off++]<<(8U*j);
        switch(tag&0xfcU) {
        case 0x04: page=value; break;
        case 0x08: assert(used<8); usage[used++]=value; break;
        case 0x74: size=value; break;
        case 0x94: field_count=value; break;
        case 0x80:
            for(unsigned j=0;j<used && j<field_count;++j) {
                unsigned axis=4;
                if(page==1 && usage[j]==0x30)axis=0;
                if(page==1 && usage[j]==0x31)axis=1;
                if(page==1 && usage[j]==0x38)axis=2;
                if(page==0x0c && usage[j]==0x238)axis=3;
                if(axis<4) {
                    assert(size==8 && (value&7U)==6U && bit+j*size==8U*(axis+1U));
                    assert(!(found&(1U<<axis))); found|=1U<<axis;
                }
            }
            bit+=size*field_count; break;
        default: break;
        }
        if((tag&0x0cU)==0)used=0; /* Main items consume local usages. */
    }
    assert(found==15 && bit==40);
}
static void descriptors_test(void)
{
    uint16_t n;
    uint8_t *p=c77_hid_class.GetHSConfigDescriptor(&n); desc_check(p,n,2,4);
    p=c77_hid_class.GetFSConfigDescriptor(&n); desc_check(p,n,2,1);
    c77_usb_device.dev_speed=USBD_SPEED_HIGH;
    p=c77_hid_class.GetOtherSpeedConfigDescriptor(&n); desc_check(p,n,7,1);
    c77_usb_device.dev_speed=USBD_SPEED_FULL;
    p=c77_hid_class.GetOtherSpeedConfigDescriptor(&n); desc_check(p,n,7,4);
    p=c77_hid_class.GetDeviceQualifierDescriptor(&n); assert(n==10 && p[1]==6 && p[7]==64 && p[8]==1);
    bits_check(keyboard_report_desc,sizeof keyboard_report_desc,256,0);
    bits_check(mouse_report_desc,sizeof mouse_report_desc,40,0);
    mouse_axes_check();
    bits_check(consumer_report_desc,sizeof consumer_report_desc,16,0);
    bits_check(vendor_report_desc,sizeof vendor_report_desc,512,512);
    p=c77_descriptors.GetSerialStrDescriptor(USBD_SPEED_HIGH,&n);
    assert(n==50); const char expected[]="0123456789ABCDEFFACE0123";
    /* 96-bit serial, no truncation or hash collisions introduced by firmware. */
    for(unsigned i=0;i<24;++i) assert(p[2+2*i]==(uint8_t)expected[i] && p[3+2*i]==0);
    for(uint16_t iface=0;iface<4;++iface) {
        assert(request(0x81,6,0x2100,iface,255)==USBD_OK && ctl_size==9 && control[1]==0x21);
        assert(request(0x81,6,0x2200,iface,255)==USBD_OK && ctl_size>20);
    }
    assert(request(0x81,6,0x2200,4,64)==USBD_FAIL);
    assert(request(0x81,6,0x2201,3,64)==USBD_FAIL);
    c77_usb_device.dev_speed=USBD_SPEED_HIGH;
}
static void control_test(void)
{
    uint8_t result[64];
    for(uint16_t iface=0;iface<4;++iface)
        assert(request(0x21,SET_REPORT,0x0200,iface,64)==(iface==3?USBD_OK:USBD_FAIL));
    const uint16_t bad_sizes[]={0,1,63,65,65535};
    for(unsigned i=0;i<sizeof bad_sizes/sizeof bad_sizes[0];++i)
        assert(request(0x21,SET_REPORT,0x0200,3,bad_sizes[i])==USBD_FAIL);
    assert(request(0x21,SET_REPORT,0x0201,3,64)==USBD_FAIL);
    assert(request(0x21,SET_REPORT,0x0300,3,64)==USBD_FAIL);
    assert(request(0x22,SET_REPORT,0x0200,3,64)==USBD_FAIL);
    assert(request(0x21,SET_REPORT,0x0200,0x103,64)==USBD_FAIL);
    assert(request(0x21,SET_REPORT,0x0200,3,64)==USBD_OK);
    memset(control_rx,0xA7,64); rx_size=64;
    assert(c77_hid_class.EP0_RxReady(&c77_usb_device)==USBD_OK);
    assert(usb_vendor_read(result) && result[0]==0xA7 && result[63]==0xA7);
    assert(request(0x21,SET_REPORT,0x0200,3,64)==USBD_OK);
    rx_size=63; c77_hid_class.EP0_RxReady(&c77_usb_device); assert(!usb_vendor_read(result));
    rx_size=64;
    assert(request(0x21,SET_REPORT,0x0200,3,64)==USBD_OK);
    usb_device_setup_begin(); c77_hid_class.EP0_RxReady(&c77_usb_device); assert(!usb_vendor_read(result));
    assert(request(0xA1,GET_REPORT,0x0100,3,65535)==USBD_OK && ctl_size==64);
    for(unsigned i=0;i<64;++i)assert(control[i]==0);
    assert(request(0xA1,GET_REPORT,0x0200,3,64)==USBD_FAIL);
    assert(request(0x21,0x0B,0,0,0)==USBD_FAIL); /* No boot switching */
    c77_usb_device.dev_state=USBD_STATE_ADDRESSED;
    assert(request(0x21,SET_REPORT,0x0200,3,64)==USBD_FAIL);
    c77_usb_device.dev_state=USBD_STATE_CONFIGURED;
}
static void queue_test(void)
{
    uint8_t result[64]; uint32_t old_drops=usb_vendor_dropped();
    for(unsigned i=0;i<6;++i) { memset(rx_irq,(int)i,64); c77_hid_class.DataOut(&c77_usb_device,4); }
    assert(usb_vendor_dropped()==old_drops+2);
    memset(rx_irq,99,64);
    for(unsigned i=0;i<4;++i) {assert(usb_vendor_read(result)); assert(result[0]==i && result[63]==i);}
    assert(!usb_vendor_read(result));
    rx_size=1; c77_hid_class.DataOut(&c77_usb_device,4); rx_size=64; assert(!usb_vendor_read(result));
    c77_hid_class.DataOut(&c77_usb_device,4);
    uint32_t gen=usb_session_generation(); usb_device_bus_event();
    assert(usb_session_generation()==gen+1 && !usb_vendor_read(result));
}
static void send_test(void)
{
    uint8_t k[32]={0},v[64]={0}; k[4/8]|=1U<<(4%8); k[0xE0/8]|=1U<<(0xE0%8);
    assert(!usb_keyboard_send(k)); usb_device_poll(); complete_all();
    assert(usb_keyboard_send(k)); assert(sent[0][0]==16 && sent[0][28]==1);
    k[0]=0; assert(sent[0][0]==16 && !usb_keyboard_send(k)); /* Persistent TX copy */
    assert(request(0xA1,GET_REPORT,0x0100,0,32)==USBD_OK && control[0]==16);
    complete_all(); assert(usb_mouse_send(3,1,-1));
    assert(sent[1][0]==3&&sent[1][1]==0&&sent[1][2]==0&&sent[1][3]==1&&sent[1][4]==255);
    assert(request(0xA1,GET_REPORT,0x0100,1,65535)==USBD_OK && ctl_size==5 && control[0]==3);
    for(unsigned i=1;i<5;++i)assert(control[i]==0); /* Never replay relative axes. */
    complete_all(); assert(request(0x21,SET_IDLE,0x0100,1,0)==USBD_OK);
    fake_tick+=4;usb_device_poll();assert(sent[1][0]==3);
    for(unsigned i=1;i<5;++i)assert(sent[1][i]==0);
    complete_all();assert(usb_mouse_send(255,-127,127));
    assert(sent[1][0]==31 && sent[1][1]==0 && sent[1][2]==0 && sent[1][3]==129 && sent[1][4]==127);
    complete_all();assert(usb_mouse_send(0,0,0));assert(sent[1][0]==0);
    assert(usb_vendor_send(v) && !usb_vendor_idle()); complete_all(); assert(usb_vendor_idle());
    fake_ipsr=1; assert(!usb_keyboard_send(k) && !usb_vendor_send(v)); fake_ipsr=0;
    fake_basepri=0x30; assert(usb_keyboard_send(k)); assert(fake_basepri==0x30); fake_basepri=0;
    complete_all(); transmit_result=USBD_BUSY; assert(!usb_keyboard_send(k)); transmit_result=USBD_OK; assert(usb_keyboard_send(k));
    complete_all(); assert(request(0x21,SET_IDLE,0x0100,0,0)==USBD_OK);
    uint32_t n=sends[0]; fake_tick+=4; usb_device_poll(); assert(sends[0]==n+1);
    complete_all(); assert(request(0x21,SET_IDLE,0x0100,3,0)==USBD_FAIL);
    assert(request(0x02,USB_REQ_CLEAR_FEATURE,0,0x81,0)==USBD_OK && flushes==1);
    assert(!usb_keyboard_send(k)); usb_device_poll(); assert(sent[0][0]==0 && sent[0][28]==0);
    c77_usb_device.dev_state=USBD_STATE_SUSPENDED;
    assert(!usb_configured() && !usb_vendor_send(v));
    c77_usb_device.dev_state=USBD_STATE_CONFIGURED;
    uint32_t old_session=usb_session_generation();
    usb_device_transport_failed();
    assert(!usb_configured() && usb_session_generation()!=old_session);
    usb_device_poll(); assert(!usb_configured());
    fake_tick+=19; usb_device_poll(); assert(!usb_configured());
    ++fake_tick; usb_device_poll(); assert(usb_configured());
    assert(!usb_keyboard_send(k)); usb_device_poll(); complete_all();
    assert(sent[0][0]==0 && sent[0][28]==0);
    usb_device_stop(); assert(!usb_configured());
}
static void stream_test(void)
{
    usb_device_init();assert(usb_configured());
    uint8_t a[32]={0},b[32]={0},c[32]={0},done[32];uint32_t done_count=0;
    a[0]=16;b[0]=32;c[0]=64;
    assert(!usb_keyboard_publish(a)); /* Every new session starts neutral. */
    usb_device_poll();
    assert(busy[0] && sent[0][0]==0);
    uint32_t n=sends[0],counter=usb_keyboard_completed();
    assert(usb_keyboard_publish(a));
    a[0]=8; /* Caller's mutable stack storage is not HAL's in-flight buffer. */
    assert(sends[0]==n && tx[0][0]==0);
    complete_keyboard_irq();
    assert(sends[0]==n+1 && tx[0][0]==16 && sent[0][0]==16 && busy[0]);
    assert(usb_keyboard_take_completed(done,&done_count));
    assert(done_count==counter+1 && done[0]==0);
    assert(!usb_keyboard_take_completed(done,&done_count));
    assert(usb_keyboard_publish(b) && usb_keyboard_publish(c));
    b[0]=c[0]=0;
    assert(sent[0][0]==16 && tx[0][0]==16); /* Never alter in-flight A. */
    complete_keyboard_irq();
    assert(sent[0][0]==64 && busy[0]); /* B was superseded, no backlog replay. */
    assert(usb_keyboard_take_completed(done,&done_count) && done[0]==16);
    assert(!usb_keyboard_send(a)); /* Legacy sends cannot bypass the mailbox. */

    /* Successful completions rearm immediately, even with the main tick
     * unchanged. This tests scheduling logic, not actual 8kHz bus timing. */
    n=sends[0];counter=usb_keyboard_completed();
    for(unsigned i=0;i<8000;i++) complete_keyboard_irq();
    assert(sends[0]==n+8000 && usb_keyboard_completed()==counter+8000);
    assert(usb_keyboard_take_completed(done,&done_count) && done[0]==64);
    assert(!usb_keyboard_take_completed(done,&done_count));

    usb_keyboard_stats_t stats,after;
    usb_keyboard_stats(&stats);
    fake_tick+=3;complete_keyboard_irq();
    assert(sent[0][0]==0); /* A stalled main cannot hold an old key forever. */
    complete_keyboard_irq();usb_keyboard_stats(&after);
    assert(after.stale_neutralizations==stats.stale_neutralizations+1);
    assert(usb_keyboard_publish(a));
    complete_keyboard_irq();assert(sent[0][0]==8);
    fake_tick=UINT32_MAX-1U;assert(usb_keyboard_publish(a));
    fake_tick=1U;complete_keyboard_irq();assert(sent[0][0]==0); /* Tick wrap */

    assert(usb_keyboard_publish(a));
    transmit_result=USBD_BUSY;
    usb_keyboard_stats(&stats);complete_keyboard_irq();
    assert(!busy[0]);usb_keyboard_stats(&after);
    assert(after.submit_failures==stats.submit_failures+1);
    transmit_result=USBD_OK;n=sends[0];usb_device_poll();
    assert(busy[0] && sends[0]==n+1 && sent[0][0]==8);

    /* Clear-HALT invalidates the mailbox and completion state. It must not
     * cause an old pressed report to return after the mandatory neutral. */
    assert(request(0x02,USB_REQ_CLEAR_FEATURE,0,0x81,0)==USBD_OK);
    assert(!keyboard_stream && !usb_keyboard_take_completed(done,&done_count));
    assert(!usb_keyboard_publish(a));usb_device_poll();assert(sent[0][0]==0);
    complete_keyboard_irq();
    assert(!busy[0]);n=sends[0];counter=usb_keyboard_completed();
    usb_device_bus_event();complete_keyboard_irq();
    assert(sends[0]==n && usb_keyboard_completed()==counter);
    assert(!usb_keyboard_take_completed(done,&done_count));

    /* FS uses the same armed buffer; its descriptor makes the host poll1ms.
     * Do not impose a second software timer or change the report size. */
    c77_usb_device.dev_speed=USBD_SPEED_FULL;usb_device_poll();
    assert(usb_keyboard_interval_us()==1000 && usb_keyboard_publish(a));
    complete_keyboard_irq();assert(sent[0][0]==8 && busy[0]);
    c77_usb_device.dev_state=USBD_STATE_SUSPENDED;
    n=sends[0];complete_keyboard_irq();assert(sends[0]==n && !busy[0]);
    assert(!usb_keyboard_publish(a));
    fake_ipsr=1;
    assert(!usb_keyboard_publish(a) && !usb_keyboard_take_completed(done,&done_count));
    fake_ipsr=0;
    usb_device_stop();assert(!keyboard_stream && !usb_configured());
}
static void tagged_completion_test(void)
{
    usb_device_init();usb_device_poll();complete_all();
    uint8_t a[32]={1},b[32]={2},done[32];uint32_t count_done,tag;
    /* Completing one transfer returns its tag, not the latest overwritten
     * mailbox tag. That distinction prevents false release-phase ACKs. */
    assert(usb_keyboard_publish_tagged(a,10));
    assert(usb_keyboard_publish_tagged(b,11));
    complete_keyboard_irq();
    assert(usb_keyboard_take_completed_tagged(done,&count_done,&tag)&&done[0]==1&&tag==10);
    complete_keyboard_irq();
    assert(usb_keyboard_take_completed_tagged(done,&count_done,&tag)&&done[0]==2&&tag==11);
    /* An old in-flight phase remains old even if several new publications occur. */
    assert(usb_keyboard_publish_tagged(a,12));assert(usb_keyboard_publish_tagged(b,13));
    complete_keyboard_irq();
    assert(usb_keyboard_take_completed_tagged(done,&count_done,&tag)&&tag==11);
    complete_keyboard_irq();
    assert(usb_keyboard_take_completed_tagged(done,&count_done,&tag)&&tag==13);
    fake_tick+=3;complete_keyboard_irq();complete_keyboard_irq();
    assert(usb_keyboard_take_completed_tagged(done,&count_done,&tag)&&tag==0&&done[0]==0);
    assert(usb_mouse_send_tagged(1,1,-1,20));
    assert(!usb_mouse_send_tagged(0,0,0,21));
    assert(c77_hid_class.DataIn(&c77_usb_device,2)==USBD_OK);
    assert(usb_aux_take_completed(1,done,&tag)&&done[0]==1&&done[3]==1&&tag==20);
    assert(!usb_aux_take_completed(1,done,&tag));
    assert(usb_mouse_send_tagged(0,0,0,21));
    assert(c77_hid_class.DataIn(&c77_usb_device,2)==USBD_OK);
    assert(usb_aux_take_completed(1,done,&tag)&&done[0]==0&&tag==21);
    assert(usb_consumer_send_tagged(0xe9,30));
    assert(!usb_consumer_send_tagged(0,31));
    assert(c77_hid_class.DataIn(&c77_usb_device,3)==USBD_OK);
    assert(usb_aux_take_completed(2,done,&tag)&&done[0]==0xe9&&tag==30);
    assert(usb_consumer_send_tagged(0,31));
    usb_device_bus_event();complete_all();
    assert(!usb_aux_take_completed(2,done,&tag));
    assert(!usb_keyboard_take_completed_tagged(done,&count_done,&tag));
    usb_device_stop();
}
int main(void)
{
    usb_device_init(); assert(usb_configured() && usb_speed()==2);
    descriptors_test(); control_test(); queue_test(); send_test();stream_test();tagged_completion_test();
    puts("USB descriptors, control validation, queue, lifecycle, immutable TX and completion-paced latest keyboard stream: PASS");
    return 0;
}
