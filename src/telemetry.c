#include "telemetry.h"
#include "keyboard.h"
#include <string.h>

/* Keeps the translation unit non-empty in builds without telemetry. */
typedef int telemetry_translation_unit;

#if C77_TELEMETRY
#if defined(__arm__)
#define TELEMETRY_RING_PLACEMENT __attribute__((section(".telemetry"),aligned(32)))
#else
#define TELEMETRY_RING_PLACEMENT
#endif
static telemetry_record ring[TELEMETRY_CAPACITY] TELEMETRY_RING_PLACEMENT;
static uint32_t head, tail, dropped, last_read_us;
static bool armed;
static uint8_t threshold = 4;
static uint16_t min_interval_us = 1000;
static uint16_t last_value[14];
static uint32_t last_time[14];
static uint16_t have_last;

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }

static void push(uint32_t time_us, uint8_t kind, uint8_t index, uint16_t value)
{
    if (head - tail >= TELEMETRY_CAPACITY) { if (dropped != 0xFFFFFFFFu) dropped++; return; }
    telemetry_record *r = &ring[head & (TELEMETRY_CAPACITY - 1U)];
    r->time_us = time_us; r->kind = kind; r->index = index; r->value = value;
    head++;
}

void telemetry_arm(bool enable, uint8_t new_threshold, uint16_t new_interval, uint32_t now_us)
{
    armed = false;
    head = tail = dropped = 0;
    have_last = 0;
    if (!enable) return;
    threshold = new_threshold ? new_threshold : 1;
    min_interval_us = new_interval;
    last_read_us = now_us;
    armed = true;
}

bool telemetry_armed(void) { return armed; }

void telemetry_get_status(telemetry_status *out)
{
    out->armed = armed; out->threshold = threshold; out->min_interval_us = min_interval_us;
    out->dropped = dropped; out->pending = head - tail;
}

void telemetry_hall(unsigned key, uint16_t value, uint32_t time_us, bool force)
{
    if (!armed || key >= 14U) return;
    const uint16_t bit = (uint16_t)(1U << key);
    if (!force && (have_last & bit)) {
        const unsigned delta = value > last_value[key] ? value - last_value[key] : last_value[key] - value;
        if (delta < threshold || (uint32_t)(time_us - last_time[key]) < min_interval_us) return;
    }
    have_last |= bit; last_value[key] = value; last_time[key] = time_us;
    push(time_us, TELEMETRY_KIND_SAMPLE, (uint8_t)key, value);
}

void telemetry_decision(unsigned key, bool pressed, bool retriggered, uint16_t travel, uint32_t time_us)
{
    if (!armed || key >= 14U) return;
    push(time_us, TELEMETRY_KIND_DECISION, (uint8_t)key,
         (uint16_t)((travel > RT_TRAVEL_MAX ? (unsigned)RT_TRAVEL_MAX : travel) | (retriggered ? 0x4000U : 0U) | (pressed ? 0x8000U : 0U)));
}

void telemetry_usb(const uint8_t previous[32], const uint8_t current[32], uint32_t time_us)
{
    if (!armed) return;
    for (unsigned byte = 0; byte < 32U; byte++) {
        const uint8_t changed = (uint8_t)(previous[byte] ^ current[byte]);
        if (!changed) continue;
        for (unsigned bit = 0; bit < 8U; bit++) if (changed & (1U << bit)) {
            const unsigned usage = byte * 8U + bit;
            push(time_us, TELEMETRY_KIND_USB, 0, (uint16_t)(usage | ((current[byte] >> bit) & 1U ? 0x100U : 0U)));
        }
    }
}

void telemetry_poll(uint32_t now_us)
{
    if (armed && (uint32_t)(now_us - last_read_us) > TELEMETRY_IDLE_DISARM_US) telemetry_arm(false, 0, 0, now_us);
}

/* u32 now, u32 base (time of the first record, or now), u32 dropped, u8 count,
 * u8 flags (bit0 armed, bit1 more pending); then count x {i16 delta from the
 * previous record's time (0 for the first), u8 kind<<6|index, u16 value}. */
uint8_t telemetry_pack(uint8_t payload[54], uint32_t now_us)
{
    memset(payload, 0, 54);
    uint8_t count = 0;
    uint32_t base = now_us, previous = now_us;
    uint8_t *out = payload + TELEMETRY_READ_HEADER_BYTES;
    while (count < TELEMETRY_RECORDS_PER_READ && tail != head) {
        const telemetry_record *r = &ring[tail & (TELEMETRY_CAPACITY - 1U)];
        int32_t delta = 0;
        if (count == 0) base = r->time_us;
        else {
            delta = (int32_t)(r->time_us - previous);
            if (delta < -32768 || delta > 32767) break;   /* starts the next reply */
        }
        put16(out, (uint16_t)(int16_t)delta);
        out[2] = (uint8_t)((r->kind << 6) | (r->index & 0x3FU));
        put16(out + 3, r->value);
        out += TELEMETRY_RECORD_BYTES;
        previous = r->time_us;
        tail++;
        count++;
    }
    put32(payload, now_us);
    put32(payload + 4, base);
    put32(payload + 8, dropped);
    payload[12] = count;
    payload[13] = (uint8_t)((armed ? 1U : 0U) | (head != tail ? 2U : 0U));
    return count;
}

static bool frame_valid(const uint8_t q[64])
{
    if (q[2] != 1 || (q[3] & 0x80U) || !get16(q + 4) || q[6] || q[7] > 54) return false;
    if (c77_crc16(q, 62) != get16(q + 62)) return false;
    for (unsigned i = 8U + q[7]; i < 62U; i++) if (q[i]) return false;
    return true;
}

bool telemetry_command(const uint8_t q[64], uint8_t r[64], uint32_t now_us)
{
    if (q[0] != 'C' || q[1] != '7' || (q[3] != TELEMETRY_CONTROL_COMMAND && q[3] != TELEMETRY_READ_COMMAND)) return false;
    memset(r, 0, 64);
    r[0] = 'C'; r[1] = '7'; r[2] = 1; r[3] = (uint8_t)(q[3] | 0x80U); r[4] = q[4]; r[5] = q[5];
    uint8_t status = C77_OK, length = 0;
    if (!frame_valid(q)) status = C77_BAD_PACKET;
    else if (q[3] == TELEMETRY_CONTROL_COMMAND) {
        if (q[7] == 4U) {
            const uint16_t interval = get16(q + 10);
            if (q[8] > 1U || (q[8] && (!q[9] || interval > 10000U))) status = C77_BAD_CONFIG;
            else telemetry_arm(q[8] != 0, q[9], interval, now_us);
        } else if (q[7] != 0U) status = C77_BAD_PACKET;
        if (!status) {
            uint8_t *out = r + 8;
            out[0] = armed; out[1] = threshold; put16(out + 2, min_interval_us);
            put32(out + 4, TELEMETRY_CAPACITY); put32(out + 8, now_us); put32(out + 12, dropped);
            put32(out + 16, head - tail);
            length = 20;
        }
    } else {
        if (q[7] != 0U) status = C77_BAD_PACKET;
        else if (!armed && head == tail) status = C77_NOT_READY;
        else {
            last_read_us = now_us;
            const uint8_t count = telemetry_pack(r + 8, now_us);
            length = (uint8_t)(TELEMETRY_READ_HEADER_BYTES + count * TELEMETRY_RECORD_BYTES);
        }
    }
    r[6] = status; r[7] = length;
    put16(r + 62, c77_crc16(r, 62));
    return true;
}
#endif
