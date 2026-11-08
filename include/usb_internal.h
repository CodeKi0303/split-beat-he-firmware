#ifndef C77_USB_INTERNAL_H
#define C77_USB_INTERNAL_H
#include "usbd_core.h"
extern USBD_HandleTypeDef c77_usb_device;
extern USBD_ClassTypeDef c77_hid_class;
extern USBD_DescriptorsTypeDef c77_descriptors;
/* IRQ-side lifecycle hook: clear pending data and invalidate the output session. */
void usb_device_bus_event(void);
void usb_device_setup_begin(void);
/* Driver failed to quiesce an endpoint: defer a detached restart to main. */
void usb_device_transport_failed(void);
#endif
