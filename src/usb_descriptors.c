#include "usb_internal.h"
#include "usbd_ctlreq.h"
#include "hardware_profile.h"

/* Development identifiers only: a shipped product needs allocated USB IDs. */
static uint8_t device_desc[] __attribute__((aligned(4))) = {
    18, USB_DESC_TYPE_DEVICE, 0, 2, 0, 0, 0, 64,
    0x83, 0x04, 0x44, 0x57,
#if FW_UPDATE_BOOT
    3, 3, /* Keep permanent boot descriptor unchanged. */
#else
    4, 3,
#endif
    USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR, 1
};
static uint8_t language[] = {4, USB_DESC_TYPE_STRING, 0x09, 0x04};
static uint8_t text_desc[USBD_MAX_STR_DESC_SIZ] __attribute__((aligned(4)));
static uint8_t *device(USBD_SpeedTypeDef speed, uint16_t *n)
{ (void)speed; *n = sizeof device_desc; return device_desc; }
static uint8_t *lang(USBD_SpeedTypeDef speed, uint16_t *n)
{ (void)speed; *n = sizeof language; return language; }
static uint8_t *string(const char *s, uint16_t *n)
{ USBD_GetString((uint8_t *)s, text_desc, n); return text_desc; }
static uint8_t *manufacturer(USBD_SpeedTypeDef speed, uint16_t *n)
{ (void)speed; return string(HARDWARE_NAME " Keyboard Development", n); }
static uint8_t *product(USBD_SpeedTypeDef speed, uint16_t *n)
{
    (void)speed;
#if FW_UPDATE_BOOT
    return string(HARDWARE_NAME " USB Firmware Recovery", n);
#else
    return string(HARDWARE_NAME " Hall Keyboard", n);
#endif
}
static uint8_t *serial(USBD_SpeedTypeDef speed, uint16_t *n)
{
    (void)speed;
    const uint32_t uid[] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    const char hex[] = "0123456789ABCDEF";
    char s[25];
    for (unsigned i = 0; i < 24; ++i)
        s[i] = hex[(uid[i / 8] >> (28 - 4 * (i % 8))) & 15U];
    s[24] = 0;
    return string(s, n);
}
static uint8_t *config(USBD_SpeedTypeDef speed, uint16_t *n)
{ (void)speed; return string("NKRO, Mouse, Consumer and WebHID", n); }
static uint8_t *interface(USBD_SpeedTypeDef speed, uint16_t *n)
{ (void)speed; return string(HARDWARE_NAME " calibration control", n); }
USBD_DescriptorsTypeDef c77_descriptors = {
    device, lang, manufacturer, product, serial, config, interface
};
