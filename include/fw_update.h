#ifndef C86_FW_UPDATE_H
#define C86_FW_UPDATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hardware_profile.h"

/* Only the permanent C86 boot build may link an enabled flash writer. */
#ifndef FW_UPDATE_BOOT
#define FW_UPDATE_BOOT 0
#endif
#define FW_UPDATE_PROTOCOL 1U
/* Updater image version layout: major[31:24], minor[23:16], patch[15:0]. */
#define FW_UPDATE_VERSION 0x00030022UL
/* Boot identity compatibility is independent of later application versions. */
#define FW_UPDATE_BOOT_VERSION 0x00030000UL
#define FW_UPDATE_BOOT_BASE 0x08000000UL
#define FW_UPDATE_APP_BASE 0x08020000UL
#define FW_UPDATE_SLOT_END 0x08060000UL
#define FW_UPDATE_METADATA_BASE 0x0805FFE0UL
#define FW_UPDATE_MAX_IMAGE 0x0003FFE0UL
#define FW_UPDATE_IDENTITY_OFFSET 0x400UL
#define FW_UPDATE_IDENTITY_BYTES 32U
#define FW_UPDATE_MIN_IMAGE (FW_UPDATE_IDENTITY_OFFSET + FW_UPDATE_IDENTITY_BYTES)
#define FW_UPDATE_FLASH_WORD 32U
#define FW_UPDATE_METADATA_MAGIC 0x33505546UL /* FUP3 */
#define FW_UPDATE_IDENTITY_MAGIC 0x33444946UL /* FID3 */

enum {
    FW_UPDATE_INFO=0x30, FW_UPDATE_ENTER=0x31, FW_UPDATE_BEGIN=0x32,
    FW_UPDATE_WRITE=0x33, FW_UPDATE_COMMIT=0x34, FW_UPDATE_RUN=0x35,
    FW_UPDATE_READ=0x36
};
enum { FW_UPDATE_MODE_BOOT=0, FW_UPDATE_MODE_APP=1 };
enum { FW_UPDATE_ACTION_NONE=0, FW_UPDATE_ACTION_ENTER=1, FW_UPDATE_ACTION_RUN=2 };
/* Values intentionally agree with the existing C7 protocol statuses. */
enum {
    FW_UPDATE_OK=0, FW_UPDATE_BAD_COMMAND=1, FW_UPDATE_BAD_PACKET=2,
    FW_UPDATE_BAD_INDEX=3, FW_UPDATE_BAD_CONFIG=4, FW_UPDATE_BUSY=5,
    FW_UPDATE_NOT_READY=7, FW_UPDATE_STORAGE_ERROR=8
};
typedef struct {
    uint32_t size;
    uint32_t crc32;
    uint32_t version;
} fw_update_image;

/* Init may validate the full application CRC once. Call before acquisition and
 * downstream power are enabled. INFO afterwards uses this checked cache.
 * handle/take_action are main-only and strictly serialized by the caller. */
void fw_update_init(void);
bool fw_update_handle(const uint8_t request[64], uint8_t reply[64], uint8_t mode);
/* Consume ONLY after the reply has actually completed on USB/UART. The caller
 * owns reset/cookie policy; no handler resets, changes outputs, or writes boot. */
unsigned fw_update_take_action(void);
/* Disconnect/cancel drops an unacknowledged reset without losing an active
 * boot transfer. It does not erase, reset, or reinitialize the receiver. */
void fw_update_cancel_action(void);
bool fw_update_app_valid(uint32_t *stack, uint32_t *reset);
bool fw_update_boot_present(void);
uint16_t fw_update_crc16(const uint8_t *bytes, size_t length);
uint32_t fw_update_crc32(const uint8_t *bytes, size_t length);

#endif
