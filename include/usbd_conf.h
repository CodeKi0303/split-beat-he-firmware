#ifndef C77_USBD_CONF_H
#define C77_USBD_CONF_H
#include "stm32h7xx_hal.h"
#include <string.h>
#include <stddef.h>
#define USBD_MAX_NUM_INTERFACES 4U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 128U
#define USBD_MAX_SUPPORTED_CLASS 1U
#define USBD_DEBUG_LEVEL 0U
#define USBD_LPM_ENABLED 0U
#define USBD_SELF_POWERED 0U
/* USB2 units are 2 mA. The board keeps split power off before configuration. */
#define USBD_MAX_POWER 250U
#define DEVICE_FS 0U
#define DEVICE_HS 1U
#define USBD_malloc USBD_static_malloc
#define USBD_free USBD_static_free
#define USBD_memset memset
#define USBD_memcpy memcpy
#define USBD_Delay HAL_Delay
#define USBD_UsrLog(...) do {} while (0)
#define USBD_ErrLog(...) do {} while (0)
#define USBD_DbgLog(...) do {} while (0)
void *USBD_static_malloc(uint32_t size);
void USBD_static_free(void *p);
#endif
