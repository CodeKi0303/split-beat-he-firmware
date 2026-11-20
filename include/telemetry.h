#ifndef C86_TELEMETRY_H
#define C86_TELEMETRY_H
/* Opt-in input telemetry for the C86 left master: Hall samples, press/release
 * decisions and delivered keyboard-report changes, stamped with the device
 * microsecond clock. Read-only with respect to input: it never changes RT
 * state, output or settings. RAM only, off at boot, disarmed by the host or
 * automatically when reads stop. */
#include <stdbool.h>
#include <stdint.h>
#include "hardware_profile.h"
#if HARDWARE_REVISION == 86 && defined(ROLE_LEFT)
#if ROLE_LEFT
#define C77_TELEMETRY 1
#endif
#endif
#ifndef C77_TELEMETRY
#define C77_TELEMETRY 0
#endif

#define TELEMETRY_CONTROL_COMMAND 0x4AU
#define TELEMETRY_READ_COMMAND 0x4BU
#define TELEMETRY_CAPABILITY 0x100000U     /* 0x80000 is fine travel (0.3.43) */
#define TELEMETRY_CAPACITY 16384U          /* records, power of two */
#define TELEMETRY_IDLE_DISARM_US 3000000U  /* no READ for this long disarms */
#define TELEMETRY_RECORDS_PER_READ 8U
#define TELEMETRY_RECORD_BYTES 5U
#define TELEMETRY_READ_HEADER_BYTES 14U

enum {
    TELEMETRY_KIND_SAMPLE = 0,   /* index = Hall key 0..13, value = 12-bit decision input before deadband */
    TELEMETRY_KIND_DECISION = 1, /* index = Hall key, value = travel 0..10000 | 0x4000 retrigger | 0x8000 pressed */
    TELEMETRY_KIND_USB = 2,      /* value = HID usage | 0x100 pressed, at completion of the first report carrying it */
};

typedef struct {
    uint32_t time_us;
    uint8_t kind;
    uint8_t index;
    uint16_t value;
} telemetry_record;

typedef struct {
    bool armed;
    uint8_t threshold;       /* minimum ADC change between samples of one key */
    uint16_t min_interval_us;/* minimum time between samples of one key */
    uint32_t dropped;        /* records lost to a full ring since arming */
    uint32_t pending;        /* records waiting to be read */
} telemetry_status;

void telemetry_arm(bool enable, uint8_t threshold, uint16_t min_interval_us, uint32_t now_us);
void telemetry_get_status(telemetry_status *out);
bool telemetry_armed(void);
/* Main context only. */
void telemetry_hall(unsigned key, uint16_t value, uint32_t time_us, bool force);
void telemetry_decision(unsigned key, bool pressed, bool retriggered, uint16_t travel, uint32_t time_us);
void telemetry_usb(const uint8_t previous[32], const uint8_t current[32], uint32_t time_us);
void telemetry_poll(uint32_t now_us);
/* Handles 0x4A/0x4B. Returns false for any other opcode. Builds a complete C7 reply. */
bool telemetry_command(const uint8_t query[64], uint8_t reply[64], uint32_t now_us);
/* Packs up to TELEMETRY_RECORDS_PER_READ records; exposed for host tests. */
uint8_t telemetry_pack(uint8_t payload[54], uint32_t now_us);
#endif
