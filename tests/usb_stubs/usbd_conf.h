#ifndef TEST_USB_CONF_H
#define TEST_USB_CONF_H
#define C77_USBD_CONF_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define USBD_MAX_NUM_INTERFACES 4U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_STR_DESC_SIZ 128U
#define USBD_MAX_SUPPORTED_CLASS 1U
#define USBD_LPM_ENABLED 0U
#define USBD_SELF_POWERED 0U
#define USBD_MAX_POWER 250U
#define DEVICE_HS 1U
#define DEVICE_FS 0U
#define __PACKED
#define __ALIGN_BEGIN
#define __ALIGN_END
#define __IO volatile
#define __STATIC_INLINE static inline
#define __attribute__(x)
#define __NVIC_PRIO_BITS 4
extern uint32_t fake_basepri, fake_ipsr, fake_tick;
static inline uint32_t __get_BASEPRI(void) { return fake_basepri; }
static inline void __set_BASEPRI_MAX(uint32_t v) { if(!fake_basepri || v<fake_basepri) fake_basepri=v; }
static inline void __set_BASEPRI(uint32_t v) { fake_basepri=v; }
static inline uint32_t __get_IPSR(void) { return fake_ipsr; }
static inline void __DSB(void) {}
static inline void __ISB(void) {}
static inline void __DMB(void) {}
static inline uint32_t HAL_GetTick(void) { return fake_tick; }
static inline uint32_t HAL_GetUIDw0(void) { return 0x01234567; }
static inline uint32_t HAL_GetUIDw1(void) { return 0x89ABCDEF; }
static inline uint32_t HAL_GetUIDw2(void) { return 0xFACE0123; }
#endif
