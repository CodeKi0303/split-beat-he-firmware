#ifndef C77_KEYBOARD_H
#define C77_KEYBOARD_H
#include "platform.h"
#include "rapid_trigger.h"
#include <stddef.h>
enum { C77_HALL=14, C77_INPUTS=28, C77_CONFIG_BYTES=476 };
enum { C77_OK=0,C77_BAD_COMMAND,C77_BAD_PACKET,C77_BAD_INDEX,C77_BAD_CONFIG,C77_BUSY,C77_CAL_INVALID,C77_NOT_READY,C77_STORAGE_ERROR };
enum { C77_BINDING_KEY_CHORD=6, C77_CAP_KEY_CHORD=0x2000 };
enum { C77_CAP_SAVE_OUTPUT=0x4000 };
typedef struct { uint16_t released,pressed,actuation,rt_press,rt_release,top_deadzone,noise,bottom_deadzone,curve_id; uint8_t flags; } c77_key_config;
/* Chords pack the first two usages into code, low byte first. A zero
 * third_key means the chord has two ordinary keys. */
typedef struct { uint16_t code; uint8_t kind,modifiers,third_key; } c77_binding;
typedef struct { c77_key_config keys[C77_HALL]; c77_binding bindings[C77_INPUTS]; } c77_config;
typedef struct {
    c77_config config;
    rt_key_t keys[C77_HALL];
    c77_sample_t sample[2];
    uint16_t filtered[C77_HALL],travel[C77_HALL],encoder_previous[4];
#if SCAN_DUAL_96K
    uint32_t filtered_q5[7];
    uint8_t filtered_q5_ready[7];
#endif
    uint32_t seen_sequence[2],pressed_mask,digital_since[6];
    uint8_t seen[2],digital_candidate[6],digital_state[6];
#if SCAN_ADC_ASYNC
    uint32_t local_bank_sequence[2],local_bank_time[2];
    uint8_t local_banks_seen,local_banks_valid;
#endif
    uint8_t output,dirty,stored_valid,save_requested;
    bool save_output, restore_output_pending;
    uint32_t generation,errors;
    uint32_t uptime_ms,uptime_anchor_us;
    uint8_t cal_key,cal_state;
    uint16_t cal_rest,cal_pressed,cal_count,cal_min,cal_max;
    uint32_t cal_sum,cal_started;
    uint8_t pending[8],pulse[8];
    uint32_t pulse_started[8];
    int32_t wheel_pending,pan_pending;
    uint32_t pulse_drops;
} c77_engine;
uint16_t c77_crc16(const uint8_t *p,size_t n);
uint32_t c77_crc32(const uint8_t *p,size_t n);
void c77_defaults(c77_config *cfg);
bool c77_config_valid(const c77_config *cfg);
void c77_config_encode(const c77_config *cfg,uint8_t out[C77_CONFIG_BYTES]);
bool c77_config_decode(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]);
void c77_engine_init(c77_engine *e,const c77_config *cfg,uint32_t generation,bool stored);
void c77_disable(c77_engine *e);
bool c77_enable_output(c77_engine *e,uint32_t now);
bool c77_source_fresh(const c77_engine *e,unsigned side,uint32_t now);
void c77_update(c77_engine *e,unsigned side,const c77_sample_t *s,uint32_t now);
#if SCAN_ADC_ASYNC
bool c77_update_left_bank(c77_engine *e,unsigned bank,const c77_sample_t *s,uint32_t now);
#endif
void c77_tick(c77_engine *e,uint32_t now);
void c77_report(const c77_engine *e,uint8_t keyboard[32],uint16_t *consumer);
/* Use the actual completed USB bitmap, not a freshly generated report. */
void c77_keyboard_report_acked(c77_engine *e,const uint8_t actual[32],uint32_t now);
/* Synchronous compatibility helper; asynchronous transports use the API above. */
void c77_keyboard_acked(c77_engine *e,uint32_t now);
void c77_consumer_acked(c77_engine *e,uint32_t now);
void c77_mouse_peek(const c77_engine *e,int8_t *wheel,int8_t *pan);
uint8_t c77_mouse_buttons(const c77_engine *e);
void c77_mouse_accepted(c77_engine *e,int8_t wheel,int8_t pan);
void c77_mouse_acked(c77_engine *e,uint8_t buttons,uint32_t now);
/* Returns false for an unrecognizable packet; otherwise produces one reply. */
bool c77_command(c77_engine *e,const uint8_t request[64],uint8_t reply[64],uint32_t now,uint8_t usb_speed,uint16_t flash_kib);
#endif
