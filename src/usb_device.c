#include "usb_device.h"
#include "usb_internal.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
#include <string.h>

#define HID_DESC 0x21U
#define REPORT_DESC 0x22U
#define GET_REPORT 0x01U
#define GET_IDLE 0x02U
#define SET_REPORT 0x09U
#define SET_IDLE 0x0AU
#define RX_DEPTH 4U
#define CONFIG_SIZE 116U
#define USB_IRQ_PRIORITY 5U
#define KEYBOARD_MAILBOX_MAX_AGE_MS 3U
#define MOUSE_REPORT_SIZE 5U

USBD_HandleTypeDef c77_usb_device;
/* No report IDs. Bit n in this 256-bit bitmap is keyboard usage n. */
static uint8_t keyboard_report_desc[] = {
    0x05,0x01,0x09,0x06,0xA1,0x01,0x05,0x07,
    0x19,0x00,0x2A,0xFF,0x00,0x15,0x00,0x25,0x01,
    0x75,0x01,0x96,0x00,0x01,0x81,0x02,0xC0
};
static uint8_t mouse_report_desc[] = {
    0x05,0x01,0x09,0x02,0xA1,0x01,0x09,0x01,0xA1,0x00,
    /* Windows mouhid expects X/Y in the Mouse collection. They are declared
     * as relative data axes but always sent as zero: this device only supplies
     * five held buttons, wheel and pan, never pointer movement. */
    0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x05,0x81,0x02,0x75,0x03,0x95,0x01,0x81,0x03,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,
    0x15,0x81,0x25,0x7F,0x75,0x08,0x95,0x03,0x81,0x06,
    0x05,0x0C,0x0A,0x38,0x02,0x95,0x01,0x81,0x06,0xC0,0xC0
};
static uint8_t consumer_report_desc[] = {
    0x05,0x0C,0x09,0x01,0xA1,0x01,0x15,0x00,0x26,0xFF,0x03,
    0x19,0x00,0x2A,0xFF,0x03,0x75,0x10,0x95,0x01,0x81,0x00,0xC0
};
static uint8_t vendor_report_desc[] = {
    0x06,0x60,0xFF,0x09,0x61,0xA1,0x01,0x15,0x00,0x26,0xFF,0x00,
    0x75,0x08,0x95,0x40,0x09,0x62,0x81,0x02,0x09,0x63,0x91,0x02,0xC0
};
#define IFACE(n,eps,sz) 9,4,n,0,eps,3,0,0,0, 9,HID_DESC,0x11,0x01,0,1,REPORT_DESC,sz,0
#define EP(addr,size,interval) 7,5,addr,3,size,0,interval
#define CONFIG(type,vinterval) \
    9,type,CONFIG_SIZE,0,4,1,0,0x80,USBD_MAX_POWER, \
    IFACE(0,1,sizeof keyboard_report_desc),EP(0x81,32,1), \
    IFACE(1,1,sizeof mouse_report_desc),EP(0x82,MOUSE_REPORT_SIZE,1), \
    IFACE(2,1,sizeof consumer_report_desc),EP(0x83,2,1), \
    IFACE(3,2,sizeof vendor_report_desc),EP(0x84,64,vinterval),EP(0x04,64,vinterval)
static uint8_t hs_desc[] = {CONFIG(USB_DESC_TYPE_CONFIGURATION,4)};
static uint8_t fs_desc[] = {CONFIG(USB_DESC_TYPE_CONFIGURATION,1)};
static uint8_t other_hs[] = {CONFIG(USB_DESC_TYPE_OTHER_SPEED_CONFIGURATION,4)};
static uint8_t other_fs[] = {CONFIG(USB_DESC_TYPE_OTHER_SPEED_CONFIGURATION,1)};
static uint8_t qualifier[] = {10,USB_DESC_TYPE_DEVICE_QUALIFIER,0,2,0,0,0,64,1,0};
_Static_assert(sizeof hs_desc == CONFIG_SIZE, "USB configuration size");

static uint8_t rx_irq[64] __attribute__((aligned(4)));
static uint8_t rx_control[64] __attribute__((aligned(4)));
static uint8_t control_reply[64] __attribute__((aligned(4)));
static uint8_t rx_queue[RX_DEPTH][64] __attribute__((aligned(4)));
static uint8_t tx[4][64] __attribute__((aligned(4)));
static uint8_t current[3][32];
static volatile uint8_t busy[4], head, tail, count, control_pending;
static volatile uint8_t class_active, neutral_mask;
static volatile uint32_t dropped, completed, session;
/* A published state is distinct from the HAL-owned in-flight tx[0]. Only the
 * completion IRQ may recycle tx[0]; main never overwrites an active transfer. */
static uint8_t keyboard_latest[32],keyboard_completed_report[32];
static uint32_t keyboard_latest_tag,keyboard_completed_tag,tx_tags[3];
static uint8_t aux_completed_report[2][5];
static uint32_t aux_completed_tag[2];
static volatile uint8_t aux_completion_pending;
static volatile uint8_t keyboard_stream,keyboard_completion_pending,keyboard_stale;
static volatile uint32_t keyboard_published_at,keyboard_published;
static volatile uint32_t keyboard_submit_failures,keyboard_stale_neutralizations;
static uint8_t idle_rate[4];
static uint32_t last_tx_ms[3];
static bool started;
static volatile uint8_t recovery;
static uint32_t recovery_started_ms;
static bool keyboard_arm_locked(void);

/* Main code masks only USB and less urgent IRQs; scan/link IRQs may preempt. */
static uint32_t lock(void)
{
    uint32_t prior = __get_BASEPRI();
    __set_BASEPRI_MAX(USB_IRQ_PRIORITY << (8U - __NVIC_PRIO_BITS));
    __DSB(); __ISB();
    return prior;
}
static void unlock(uint32_t prior)
{ __DMB(); __set_BASEPRI(prior); }

void usb_device_bus_event(void)
{
    ++session;
    head = tail = count = control_pending = 0;
    memset((void *)busy, 0, sizeof busy);
    memset(tx, 0, sizeof tx);
    memset(current, 0, sizeof current);
    memset(keyboard_latest,0,sizeof keyboard_latest);
    memset(keyboard_completed_report,0,sizeof keyboard_completed_report);
    keyboard_stream=keyboard_completion_pending=keyboard_stale=0;
    keyboard_published_at=0;
    keyboard_latest_tag=keyboard_completed_tag=0;aux_completion_pending=0;
    memset(tx_tags,0,sizeof tx_tags);
    neutral_mask = 7U;
}
void usb_device_setup_begin(void) { control_pending = 0; }
void usb_device_transport_failed(void)
{
    class_active = 0;
    usb_device_bus_event();
    recovery = 1;
}

static uint8_t fail(USBD_HandleTypeDef *dev, USBD_SetupReqTypedef *req)
{ USBD_CtlError(dev, req); return USBD_FAIL; }
static uint8_t *config_hs(uint16_t *n) { *n = sizeof hs_desc; return hs_desc; }
static uint8_t *config_fs(uint16_t *n) { *n = sizeof fs_desc; return fs_desc; }
static uint8_t *config_other(uint16_t *n)
{
    *n = sizeof other_fs;
    return c77_usb_device.dev_speed == USBD_SPEED_HIGH ? other_fs : other_hs;
}
static uint8_t *qualifier_get(uint16_t *n) { *n = sizeof qualifier; return qualifier; }
static uint8_t class_init(USBD_HandleTypeDef *dev, uint8_t cfg)
{
    (void)cfg;
    usb_device_bus_event();
    memset(idle_rate, 0, sizeof idle_rate);
    class_active = 1;
    for (uint8_t i = 1; i <= 4; ++i) {
        uint16_t size = i == 1 ? 32 : i == 4 ? 64 : i == 2 ? MOUSE_REPORT_SIZE : 2;
        if (USBD_LL_OpenEP(dev, 0x80U | i, USBD_EP_TYPE_INTR, size) != USBD_OK)
            return USBD_FAIL;
        dev->ep_in[i].is_used = 1;
        dev->ep_in[i].bInterval = i == 4 && dev->dev_speed == USBD_SPEED_HIGH ? 4 : 1;
    }
    if (USBD_LL_OpenEP(dev, 4, USBD_EP_TYPE_INTR, 64) != USBD_OK) return USBD_FAIL;
    dev->ep_out[4].is_used = 1;
    dev->ep_out[4].bInterval = dev->dev_speed == USBD_SPEED_HIGH ? 4 : 1;
    return (uint8_t)USBD_LL_PrepareReceive(dev, 4, rx_irq, sizeof rx_irq);
}
static uint8_t class_deinit(USBD_HandleTypeDef *dev, uint8_t cfg)
{
    (void)cfg;
    class_active = 0;
    usb_device_bus_event();
    for (uint8_t i = 1; i <= 4; ++i) {
        (void)USBD_LL_CloseEP(dev, 0x80U | i);
        dev->ep_in[i].is_used = 0;
    }
    (void)USBD_LL_CloseEP(dev, 4);
    dev->ep_out[4].is_used = 0;
    return USBD_OK;
}
static uint8_t setup(USBD_HandleTypeDef *dev, USBD_SetupReqTypedef *req)
{
    control_pending = 0; /* A new SETUP aborts an incomplete control write. */
    /* ST Core has already cleared endpoint HALT and submitted status. It
     * forwards this standard endpoint request to the class for recovery. */
    if (req->bmRequest == 0x02 && req->bRequest == USB_REQ_CLEAR_FEATURE &&
        req->wValue == USB_FEATURE_EP_HALT && req->wLength == 0 &&
        dev->dev_state == USBD_STATE_CONFIGURED) {
        if (req->wIndex == 4)
            return (uint8_t)USBD_LL_PrepareReceive(dev, 4, rx_irq, 64);
        if (req->wIndex >= 0x81 && req->wIndex <= 0x84) {
            uint8_t index = (uint8_t)(req->wIndex - 0x81);
            if (USBD_LL_FlushEP(dev, (uint8_t)req->wIndex) != USBD_OK) return USBD_FAIL;
            busy[index] = 0;
            if(index==0) {
                keyboard_stream=keyboard_completion_pending=keyboard_stale=0;
                memset(keyboard_latest,0,sizeof keyboard_latest);
            }
            if (index < 3) neutral_mask |= (uint8_t)(1U << index);
            return USBD_OK;
        }
        return fail(dev, req);
    }
    if ((req->bmRequest & 0x1FU) != USB_REQ_RECIPIENT_INTERFACE || req->wIndex >= 4)
        return fail(dev, req);
    uint8_t iface = (uint8_t)req->wIndex;
    if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        if (req->bRequest == USB_REQ_GET_DESCRIPTOR && req->bmRequest == 0x81 &&
            (req->wValue & 255U) == 0) {
            uint8_t *p = NULL;
            uint16_t size = 0;
            if ((req->wValue >> 8) == REPORT_DESC) {
                uint8_t *desc[] = {keyboard_report_desc, mouse_report_desc, consumer_report_desc, vendor_report_desc};
                const uint16_t sizes[] = {sizeof keyboard_report_desc, sizeof mouse_report_desc,
                                         sizeof consumer_report_desc, sizeof vendor_report_desc};
                p = desc[iface]; size = sizes[iface];
            } else if ((req->wValue >> 8) == HID_DESC) {
                p = hs_desc + 18U + 25U * iface; size = 9;
            }
            if (!p || !req->wLength) return fail(dev, req);
            return (uint8_t)USBD_CtlSendData(dev, p, MIN(size, req->wLength));
        }
        if (dev->dev_state != USBD_STATE_CONFIGURED) return fail(dev, req);
        if (req->bRequest == USB_REQ_GET_INTERFACE && req->bmRequest == 0x81 &&
            req->wValue == 0 && req->wLength == 1) {
            control_reply[0] = 0;
            return (uint8_t)USBD_CtlSendData(dev, control_reply, 1);
        }
        if (req->bRequest == USB_REQ_SET_INTERFACE && req->bmRequest == 0x01 &&
            req->wValue == 0 && req->wLength == 0) return USBD_OK;
        if (req->bRequest == USB_REQ_GET_STATUS && req->bmRequest == 0x81 &&
            req->wValue == 0 && req->wLength == 2) {
            control_reply[0] = control_reply[1] = 0;
            return (uint8_t)USBD_CtlSendData(dev, control_reply, 2);
        }
        return fail(dev, req);
    }
    if ((req->bmRequest & USB_REQ_TYPE_MASK) != USB_REQ_TYPE_CLASS ||
        dev->dev_state != USBD_STATE_CONFIGURED) return fail(dev, req);
    if (req->bRequest == SET_REPORT && req->bmRequest == 0x21 && iface == 3 &&
        req->wValue == 0x0200 && req->wLength == sizeof rx_control) {
        control_pending = 1;
        return (uint8_t)USBD_CtlPrepareRx(dev, rx_control, sizeof rx_control);
    }
    if (req->bRequest == GET_REPORT && req->bmRequest == 0xA1 &&
        req->wValue == 0x0100 && req->wLength != 0) {
        uint16_t size = iface == 0 ? 32 : iface == 3 ? 64 : iface == 1 ? MOUSE_REPORT_SIZE : 2;
        memset(control_reply, 0, sizeof control_reply);
        /* Relative scroll must never be replayed by GET_REPORT. Vendor GET_REPORT
         * is neutral; command replies arrive only on the vendor IN endpoint. */
        if (iface == 0 || iface == 2) memcpy(control_reply, current[iface], size);
        if (iface == 1) control_reply[0] = current[1][0];
        return (uint8_t)USBD_CtlSendData(dev, control_reply, MIN(size, req->wLength));
    }
    if (req->bRequest == GET_IDLE && req->bmRequest == 0xA1 &&
        req->wValue == 0 && req->wLength == 1) {
        control_reply[0] = idle_rate[iface];
        return (uint8_t)USBD_CtlSendData(dev, control_reply, 1);
    }
    if (req->bRequest == SET_IDLE && req->bmRequest == 0x21 &&
        (req->wValue & 255U) == 0 && req->wLength == 0 &&
        (iface != 3 || req->wValue == 0)) {
        idle_rate[iface] = (uint8_t)(req->wValue >> 8);
        return USBD_OK;
    }
    /* No boot subclass, protocol switching, output LEDs, or feature reports. */
    return fail(dev, req);
}
static void receive_copy(const uint8_t *report)
{
    if (count == RX_DEPTH) { ++dropped; return; }
    memcpy(rx_queue[head], report, 64);
    head = (uint8_t)((head + 1U) % RX_DEPTH);
    __DMB();
    ++count;
}
static uint8_t control_out(USBD_HandleTypeDef *dev)
{
    if (control_pending && USBD_LL_GetRxDataSize(dev, 0) == 64)
        receive_copy(rx_control);
    else if (control_pending) ++dropped;
    control_pending = 0;
    return USBD_OK;
}
static uint8_t data_out(USBD_HandleTypeDef *dev, uint8_t ep)
{
    if (ep != 4) return USBD_FAIL;
    if (USBD_LL_GetRxDataSize(dev, ep) == 64) receive_copy(rx_irq);
    else ++dropped;
    return (uint8_t)USBD_LL_PrepareReceive(dev, ep, rx_irq, 64);
}
static uint8_t data_in(USBD_HandleTypeDef *dev, uint8_t ep)
{
    (void)dev;
    if (ep < 1 || ep > 4) return USBD_FAIL;
    /* Ignore a late callback after reset/quiesce rather than count a delivery
     * or revive a state that belongs to the previous USB session. */
    if(!busy[ep-1]) return USBD_OK;
    if(ep==1) {
        memcpy(keyboard_completed_report,tx[0],32);
        ++completed;
        keyboard_completion_pending=1;
        keyboard_completed_tag=tx_tags[0];
    }else if(ep==2||ep==3){
        unsigned i=ep-2;
        memcpy(aux_completed_report[i],tx[ep-1],ep==2?5U:2U);
        aux_completed_tag[i]=tx_tags[ep-1];
        aux_completion_pending|=(uint8_t)(1U<<i);
    }
    busy[ep - 1] = 0;
    /* ST HAL clears XFRC and disables the old FIFO-empty mask before invoking
     * this callback. Rearming here prepares the next host poll without adding
     * a second unsynchronised 125us main-loop delay. No engine runs in IRQ. */
    if(ep==1 && keyboard_stream) (void)keyboard_arm_locked();
    return USBD_OK;
}
USBD_ClassTypeDef c77_hid_class = {
    .Init=class_init, .DeInit=class_deinit, .Setup=setup, .EP0_RxReady=control_out,
    .DataIn=data_in, .DataOut=data_out, .GetHSConfigDescriptor=config_hs,
    .GetFSConfigDescriptor=config_fs, .GetOtherSpeedConfigDescriptor=config_other,
    .GetDeviceQualifierDescriptor=qualifier_get
};

bool usb_configured(void)
{ return started && class_active && c77_usb_device.dev_state == USBD_STATE_CONFIGURED; }
uint8_t usb_speed(void)
{ return usb_configured() ? (c77_usb_device.dev_speed == USBD_SPEED_HIGH ? 2 : 1) : 0; }
uint32_t usb_keyboard_interval_us(void)
{ return c77_usb_device.dev_speed == USBD_SPEED_HIGH ? 125 : 1000; }
uint32_t usb_session_generation(void) { return session; }
uint32_t usb_vendor_dropped(void) { return dropped; }
uint32_t usb_keyboard_completed(void) { return completed; }
bool usb_vendor_idle(void) { return !busy[3]; }

/* Called with USB IRQ masked from main, or by the EP1 completion IRQ itself. */
static bool send_locked(uint8_t index, const uint8_t *data, uint16_t size)
{
    if (!usb_configured() || busy[index]) return false;
    memcpy(tx[index], data, size);
    if(index<3)tx_tags[index]=0;
    busy[index] = 1;
    if (USBD_LL_Transmit(&c77_usb_device, 0x81U + index, tx[index], size) != USBD_OK) {
        busy[index] = 0;
        if(index==0) ++keyboard_submit_failures;
        return false;
    }
    if (index < 3) {
        if (index != 1) memcpy(current[index], data, size);
        else { memset(current[1],0,MOUSE_REPORT_SIZE); current[1][0] = data[0]; }
        last_tx_ms[index] = HAL_GetTick();
    }
    return true;
}
static bool keyboard_arm_locked(void)
{
    if(!keyboard_stream || (neutral_mask&1U) || !usb_configured() || busy[0]) return false;
    bool stale=(uint32_t)(HAL_GetTick()-keyboard_published_at)>=KEYBOARD_MAILBOX_MAX_AGE_MS;
    const uint8_t zero[32]={0};
    if(stale && !keyboard_stale) ++keyboard_stale_neutralizations;
    keyboard_stale=(uint8_t)stale;
    /* If main stalls while interrupts continue, never repeat held keys
     * indefinitely. New publication is needed to leave neutral fallback. */
    bool sent=send_locked(0,stale?zero:keyboard_latest,32);
    if(sent)tx_tags[0]=stale?0:keyboard_latest_tag;
    return sent;
}
static bool send(uint8_t index, const uint8_t *data, uint16_t size)
{
    if (!data || __get_IPSR() != 0) return false;
    uint32_t prior = lock();
    bool result = false;
    /* A main-loop poll must submit the neutral report after every new session. */
    if (index == 3 || !(neutral_mask & (1U << index))) result = send_locked(index, data, size);
    unlock(prior);
    return result;
}
void usb_device_poll(void)
{
    if (__get_IPSR() != 0) return;
    /* A timed-out register handshake already disconnected in the IRQ. The
     * potentially slower HAL reinitialization runs here, never in the IRQ. */
    if (recovery == 1) {
        usb_device_stop();
        recovery_started_ms = HAL_GetTick();
        recovery = 2;
        return;
    }
    if (recovery == 2) {
        if ((uint32_t)(HAL_GetTick() - recovery_started_ms) >= 20U) usb_device_init();
        return;
    }
    uint32_t prior = lock();
    const uint8_t zero[32] = {0};
    uint32_t now = HAL_GetTick();
    for (uint8_t i = 0; i < 3; ++i) {
        uint16_t size = i == 0 ? 32 : i == 1 ? MOUSE_REPORT_SIZE : 2;
        if (neutral_mask & (1U << i)) {
            if (send_locked(i, zero, size)) neutral_mask &= (uint8_t)~(1U << i);
        } else if(i==0 && keyboard_stream) {
            /* Retry an arm rejected by the lower layer. Normally DataIn has
             * already armed EP1, so this does no work or FIFO manipulation. */
            (void)keyboard_arm_locked();
        } else if (idle_rate[i] && (uint32_t)(now - last_tx_ms[i]) >= 4U * idle_rate[i]) {
            (void)send_locked(i, current[i], size);
        }
    }
    unlock(prior);
}
bool usb_keyboard_send(const uint8_t report[32])
{ return !keyboard_stream && send(0, report, 32); }
bool usb_keyboard_publish(const uint8_t report[32])
{ return usb_keyboard_publish_tagged(report,0); }
bool usb_keyboard_publish_tagged(const uint8_t report[32],uint32_t tag)
{
    if(!report || __get_IPSR()!=0) return false;
    uint32_t prior=lock();
    bool accepted=usb_configured() && !(neutral_mask&1U);
    if(accepted) {
        memcpy(keyboard_latest,report,32);
        keyboard_latest_tag=tag;
        keyboard_published_at=HAL_GetTick();keyboard_stale=0;
        keyboard_stream=1;++keyboard_published;
        (void)keyboard_arm_locked();
    }
    unlock(prior);
    return accepted;
}
bool usb_keyboard_take_completed(uint8_t report[32],uint32_t *completion_count)
{ uint32_t tag;return usb_keyboard_take_completed_tagged(report,completion_count,&tag); }
bool usb_keyboard_take_completed_tagged(uint8_t report[32],uint32_t *completion_count,uint32_t *tag)
{
    if(!report || !completion_count || !tag || __get_IPSR()!=0) return false;
    uint32_t prior=lock();
    bool available=keyboard_completion_pending!=0;
    if(available) {
        memcpy(report,keyboard_completed_report,32);
        *completion_count=completed;
        *tag=keyboard_completed_tag;
        keyboard_completion_pending=0;
    }
    unlock(prior);
    return available;
}
void usb_keyboard_stats(usb_keyboard_stats_t *out)
{
    if(!out || __get_IPSR()!=0) return;
    uint32_t prior=lock();
    out->completed=completed;out->published=keyboard_published;
    out->submit_failures=keyboard_submit_failures;
    out->stale_neutralizations=keyboard_stale_neutralizations;
    unlock(prior);
}
bool usb_mouse_send(uint8_t buttons, int8_t wheel, int8_t pan)
{ const uint8_t report[MOUSE_REPORT_SIZE] = {(uint8_t)(buttons&31u),0,0,(uint8_t)wheel,(uint8_t)pan}; return send(1, report, sizeof report); }
bool usb_consumer_send(uint16_t usage)
{ const uint8_t report[] = {(uint8_t)usage,(uint8_t)(usage >> 8)}; return send(2, report, 2); }
static bool send_tagged(uint8_t index,const uint8_t *report,uint16_t size,uint32_t tag){
    if(__get_IPSR()!=0)return false;
    uint32_t prior=lock();bool result=false;
    if(!(neutral_mask&(1U<<index))){
        result=send_locked(index,report,size);
        if(result)tx_tags[index]=tag;
    }
    unlock(prior);return result;
}
bool usb_mouse_send_tagged(uint8_t buttons,int8_t wheel,int8_t pan,uint32_t tag){
    const uint8_t report[MOUSE_REPORT_SIZE]={(uint8_t)(buttons&31u),0,0,(uint8_t)wheel,(uint8_t)pan};
    return send_tagged(1,report,sizeof report,tag);
}
bool usb_consumer_send_tagged(uint16_t usage,uint32_t tag){
    const uint8_t report[2]={(uint8_t)usage,(uint8_t)(usage>>8)};
    return send_tagged(2,report,2,tag);
}
bool usb_aux_take_completed(unsigned index,uint8_t report[5],uint32_t *tag){
    if(index<1||index>2||!report||!tag||__get_IPSR()!=0)return false;
    uint32_t prior=lock();unsigned i=index-1;uint8_t bit=(uint8_t)(1U<<i);
    bool available=(aux_completion_pending&bit)!=0;
    if(available){memcpy(report,aux_completed_report[i],index==1?5U:2U);*tag=aux_completed_tag[i];aux_completion_pending&=(uint8_t)~bit;}
    unlock(prior);return available;
}
bool usb_vendor_send(const uint8_t report[64]) { return send(3, report, 64); }
bool usb_vendor_read(uint8_t report[64])
{
    if (!report || __get_IPSR() != 0) return false;
    uint32_t prior = lock();
    bool available = count != 0;
    if (available) {
        memcpy(report, rx_queue[tail], 64);
        tail = (uint8_t)((tail + 1U) % RX_DEPTH);
        --count;
    }
    unlock(prior);
    return available;
}
void usb_device_init(void)
{
    if (started) return;
    recovery = 0;
    usb_device_bus_event();
    if (USBD_Init(&c77_usb_device, &c77_descriptors, DEVICE_HS) != USBD_OK) return;
    if (USBD_RegisterClass(&c77_usb_device, &c77_hid_class) != USBD_OK ||
        USBD_Start(&c77_usb_device) != USBD_OK) {
        (void)USBD_DeInit(&c77_usb_device);
        return;
    }
    started = true;
}
void usb_device_stop(void)
{
    recovery = 0;
    if (!started) return;
    (void)USBD_Stop(&c77_usb_device);
    (void)USBD_DeInit(&c77_usb_device);
    started = false;
    class_active = 0;
    usb_device_bus_event();
}
