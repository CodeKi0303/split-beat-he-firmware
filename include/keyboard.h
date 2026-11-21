#ifndef C77_KEYBOARD_H
#define C77_KEYBOARD_H
#include "platform.h"
#include "rapid_trigger.h"
#include <stddef.h>
enum { C77_HALL=14, C77_INPUTS=28, C77_CONFIG_BYTES=476 };
enum { C77_OK=0,C77_BAD_COMMAND,C77_BAD_PACKET,C77_BAD_INDEX,C77_BAD_CONFIG,C77_BUSY,C77_CAL_INVALID,C77_NOT_READY,C77_STORAGE_ERROR };
enum { C77_BINDING_KEY_CHORD=6, C77_CAP_KEY_CHORD=0x2000 };
enum { C77_CAP_SAVE_OUTPUT=0x4000 };
enum { C77_KEY_RETRIGGER=0x08, C77_CAP_RETRIGGER=0x40000 };
enum { C77_CAP_ADC_TUNING=0x8000,C77_CAP_ADC_PAIR=0x10000,C77_CAP_ADC_PAIR_READ=0x20000 };
/* Travel thresholds, STATUS.travel and ADC_TRACE travel use 0..10000. */
enum { C77_CAP_FINE_TRAVEL=0x80000 };
/* 0.3.46+: curve_id 2 and command 0x4C read/write the shared custom curve. */
enum { C77_CAP_CUSTOM_CURVE=0x200000, C77_OP_CUSTOM_CURVE=0x4C, C77_CURVE_BYTES=2*RT_CUSTOM_CURVE_POINTS };
/* 0.3.48+: kind 7 moves the mouse pointer. modifiers = axis (0 X, 1 Y);
 * code = signed speed. Hall keys: px/s at full depth, scaled by the reported
 * travel. MX/knob-click inputs: px/s, ramping from 30% to 100% over 350 ms.
 * Encoder directions: px per detent. */
enum { C77_BINDING_POINTER=7, C77_CAP_POINTER=0x400000, C77_POINTER_MAX=4000 };
typedef struct { uint16_t released,pressed,actuation,rt_press,rt_release,top_deadzone,noise,bottom_deadzone,curve_id; uint8_t flags; } c77_key_config;
/* Chords pack the first two usages into code, low byte first. A zero
 * third_key means the chord has two ordinary keys. */
typedef struct { uint16_t code; uint8_t kind,modifiers,third_key; } c77_binding;
/* curve is the shared RT_CURVE_CUSTOM table. It is not part of the 476-byte
 * key/binding encoding; storage and INFO config_crc append it. */
typedef struct { c77_key_config keys[C77_HALL]; c77_binding bindings[C77_INPUTS]; uint16_t curve[RT_CUSTOM_CURVE_POINTS]; } c77_config;
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
    /* One outstanding event/key: 1 await held ACK, 2 release, 3 repress.
     * Epoch tags bind ACKs to actual published phases, never mailbox intent. */
    /* Pointer motion (kind 7): whole-pixel backlog per axis, sub-pixel
     * remainder in 1e-6 px, last tick time and press start of held inputs. */
    int32_t pointer_pending[2],pointer_frac[2];
    uint32_t pointer_last,pointer_held,pointer_since[20];
    uint8_t retrigger[C77_HALL];
    uint16_t retrigger_mask;
    uint32_t retrigger_epoch, retrigger_since[C77_HALL], retrigger_tag[C77_HALL];
} c77_engine;
uint16_t c77_crc16(const uint8_t *p,size_t n);
uint32_t c77_crc32(const uint8_t *p,size_t n);
void c77_defaults(c77_config *cfg);
bool c77_config_valid(const c77_config *cfg);
void c77_config_encode(const c77_config *cfg,uint8_t out[C77_CONFIG_BYTES]);
bool c77_config_decode(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]);
/* Custom curve: 15 little-endian u16. Decode rejects a non-monotonic table. */
void c77_curve_encode(const uint16_t curve[RT_CUSTOM_CURVE_POINTS],uint8_t out[C77_CURVE_BYTES]);
bool c77_curve_decode(uint16_t curve[RT_CUSTOM_CURVE_POINTS],const uint8_t in[C77_CURVE_BYTES]);
/* CRC32 of the key/binding encoding followed by the custom curve. */
uint32_t c77_config_crc(const c77_config *cfg);
/* Decode a 0..1000-unit record written by firmware <=0.3.42 into 0..10000 units. */
bool c77_config_decode_legacy(c77_config *cfg,const uint8_t in[C77_CONFIG_BYTES]);
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
/* Integrate held pointer bindings up to now (call once per mouse report). */
void c77_pointer_tick(c77_engine *e,uint32_t now);
void c77_pointer_peek(const c77_engine *e,int8_t *x,int8_t *y);
void c77_pointer_accepted(c77_engine *e,int8_t x,int8_t y);
void c77_mouse_acked(c77_engine *e,uint8_t buttons,uint32_t now);
/* type 0 keyboard (32 bytes), 1 mouse (buttons byte), 2 consumer (LE u16).
 * Called ONLY with a completed USB transfer and the original snapshot tag. */
void c77_retrigger_report_acked(c77_engine *e,unsigned type,const uint8_t *actual,uint32_t tag,uint32_t now);
/* Returns false for an unrecognizable packet; otherwise produces one reply. */
bool c77_command(c77_engine *e,const uint8_t request[64],uint8_t reply[64],uint32_t now,uint8_t usb_speed,uint16_t flash_kib);
#endif
