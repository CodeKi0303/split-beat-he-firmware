/* Stable, inspectable image identity. This does not authenticate an image;
 * firmware packages remain trusted local/user-selected inputs. */
#include "fw_update.h"

#if HARDWARE_REVISION == 86
#ifndef ROLE_LEFT
#error ROLE_LEFT is required for a C86 image identity
#endif
struct firmware_identity {
    uint8_t magic[8];
    uint8_t hardware, role;
    uint16_t protocol;
    uint32_t version, app_base, max_image, check, check_inverse;
};
_Static_assert(sizeof(struct firmware_identity)==FW_UPDATE_IDENTITY_BYTES,
               "Firmware identity must occupy one 32-byte flashword");

__attribute__((section(".fw_identity"),used,aligned(4)))
const struct firmware_identity fw_image_identity = {
#if FW_UPDATE_BOOT
    {'C','8','6','B','L','0','0','3'},
#else
    {'C','8','6','F','W','0','0','3'},
#endif
    86U, ROLE_LEFT ? 0U : 1U, FW_UPDATE_PROTOCOL,
#if FW_UPDATE_BOOT
    FW_UPDATE_BOOT_VERSION,
#else
    FW_UPDATE_VERSION,
#endif
    FW_UPDATE_APP_BASE, FW_UPDATE_MAX_IMAGE,
    FW_UPDATE_IDENTITY_MAGIC, (uint32_t)~FW_UPDATE_IDENTITY_MAGIC
};
#endif
