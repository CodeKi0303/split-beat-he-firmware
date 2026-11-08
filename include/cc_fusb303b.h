#ifndef CC_FUSB303B_H
#define CC_FUSB303B_H
#include <stdbool.h>
#include <stdint.h>
#define FUSB303B_ADDRESS 0x21U /* 7-bit; ADDR resistively strapped LOW. */
typedef enum { CC_IO_BUSY, CC_IO_OK, CC_IO_ERROR } cc_io_result;
typedef struct {
    void *context;
    /* Keep value valid until poll() completes. Never block waiting for the bus. */
    bool (*start)(void *context,bool write,uint8_t reg,uint8_t *value);
    cc_io_result (*poll)(void *context);
    void (*cancel)(void *context);
} cc_io;
typedef enum { CC_OFF, CC_STARTING, CC_INIT, CC_READBACK, CC_CLEAR, CC_READY, CC_FAULT } cc_state;
typedef enum { CC_ERROR_NONE, CC_ERROR_POWER, CC_ERROR_BUS, CC_ERROR_TIMEOUT,
    CC_ERROR_ID, CC_ERROR_CONFIG, CC_ERROR_STATUS } cc_error;
typedef struct {
    cc_state state;
    cc_error error;
    uint32_t started_at,transaction_at,checked_at;
    uint8_t step,value,registers[10];
    bool pending,enable,allow_power,attached;
} cc_fusb303b;
void cc_fusb303b_reset(cc_fusb303b *cc,const cc_io *io);
/* configured grants only the existing upstream USB budget, never 1.5/3 A.
 * id_low is the hardware ID pin; fault is the external load-switch fault. */
void cc_fusb303b_service(cc_fusb303b *cc,const cc_io *io,uint32_t now_us,
    bool configured,bool fault,bool id_low,bool interrupt_low);
#endif
