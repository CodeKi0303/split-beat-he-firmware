#ifndef C77_USB_DEVICE_H
#define C77_USB_DEVICE_H
#include <stdint.h>
#include <stdbool.h>
void usb_device_init(void);
void usb_device_stop(void);
void usb_device_poll(void); /* main loop: neutral session reports and HID idle */
uint32_t usb_session_generation(void); /* reset/suspend/disconnect invalidates output */
bool usb_configured(void);
uint8_t usb_speed(void); /* 0disconnected 1FS 2HS */
uint32_t usb_keyboard_interval_us(void);
bool usb_keyboard_send(const uint8_t report[32]);
/* Main publishes a latest-state mailbox. EP1 completion rearms from that copy,
 * letting the USB host pace reports; publication is NOT a delivery ACK. */
bool usb_keyboard_publish(const uint8_t report[32]);
/* Latest actual EP1 completion, copied atomically. Call before input/pulse
 * state advances; acknowledge only usages represented by this report. */
bool usb_keyboard_take_completed(uint8_t report[32],uint32_t *completion_count);
typedef struct {
    /* Monotonic modulo-2^32 since boot, including neutral reports. Counts are
     * actual DataIn completions, not USB microframes or successful publish calls. */
    uint32_t completed,published,submit_failures,stale_neutralizations;
} usb_keyboard_stats_t;
void usb_keyboard_stats(usb_keyboard_stats_t *out);
bool usb_mouse_send(uint8_t buttons,int8_t wheel,int8_t pan);
bool usb_consumer_send(uint16_t usage);
bool usb_vendor_read(uint8_t report[64]);
bool usb_vendor_send(const uint8_t report[64]);
bool usb_vendor_idle(void);
uint32_t usb_vendor_dropped(void);
uint32_t usb_keyboard_completed(void);
#endif
