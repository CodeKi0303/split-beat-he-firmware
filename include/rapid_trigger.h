#ifndef RAPID_TRIGGER_H
#define RAPID_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

/* Normalized travel 0..10000 (firmware 0.3.43+, capability 0x80000; earlier
 * firmware used 0..1000). Curve 0 is ADC range; curve 1 is an approximate
 * reference distance fraction, not independently measured physical distance.
 * One unit is 1/10000 of the configured stroke (0.00034 mm at 3.4 mm), which
 * lets hosts enter 0.001 mm settings; it is finer than one ADC code. */
enum {
    RT_TRAVEL_MAX = 10000,
    RT_LEGACY_TRAVEL_MAX = 1000,
    RT_LEGACY_TRAVEL_SCALE = RT_TRAVEL_MAX / RT_LEGACY_TRAVEL_MAX,
    RT_MIN_ADC_SPAN = 128,
    RT_CURVE_LINEAR = 0,
    RT_CURVE_OWLAB_TI_REFERENCE = 1,
    RT_ADC_Q5_SCALE = 32,
    RT_ADC_Q5_MAX = 16383 * 8
};

typedef struct {
    uint16_t adc_released;
    uint16_t adc_pressed;
    uint16_t actuation;
    uint16_t rt_press;
    uint16_t rt_release;
    uint16_t top_deadzone;
    uint16_t bottom_deadzone;
    uint16_t curve_id;
    bool enabled;
    bool rapid_trigger_enabled;
    bool retrigger_before_release;
} rt_config_t;

typedef struct {
    rt_config_t config;
    /* Unclamped curve coordinate used by all RT/retrigger state transitions. */
    uint16_t travel;
    uint16_t extreme;
    bool pressed;
    bool rapid_cycle;
    bool valid;
    uint16_t retrigger_valley;
    bool retrigger_armed;
    /* Private normalization cache, never serialized. Configure/reset invalidate
     * it; a cache hit still runs the complete RT state machine. */
    uint8_t normalization_kind;
    uint32_t normalization_input;
    uint16_t normalization_travel;
} rt_key_t;

typedef struct {
    /* Reported coordinate: top band -> 0, bottom band -> RT_TRAVEL_MAX; middle unchanged. */
    uint16_t travel;
    bool pressed;
    bool changed;
    /* A synthetic repeat request; physical pressed and changed stay unchanged. */
    bool retriggered;
} rt_result_t;

/* Defaults: initial actuation 2000; RT press/release 500; top deadzone 200;
 * bottom deadzone 0 (disabled), curve 0 (linear, legacy-compatible).
 * Both key processing and rapid trigger are enabled. ADC polarity may be
 * either direction. Endpoints must be measured by the caller. */
void rt_config_defaults(rt_config_t *config, uint16_t adc_released,
                        uint16_t adc_pressed);

/* Configure only outside concurrent processing (e.g. pause the sample IRQ).
 * Every call releases the key and resets the initial-actuation state.
 * Invalid configuration disables processing and returns false. Required:
 * abs(ADC span) >= 128; top_deadzone < actuation <= RT_TRAVEL_MAX;
 * 0 <= bottom_deadzone <= RT_TRAVEL_MAX - actuation;
 * curve_id is RT_CURVE_LINEAR or RT_CURVE_OWLAB_TI_REFERENCE;
 * 1 <= rt_press, rt_release <= RT_TRAVEL_MAX. */
bool rt_key_configure(rt_key_t *key, const rt_config_t *config);

/* Preserve configuration while releasing/resetting the key. The caller must
 * publish the new released state; reset/configure do not return an edge. */
void rt_key_reset(rt_key_t *key);

/* Call once per sample. No filtering, heap, floating point, or HAL calls.
 * Identical ADC input reuses normalization only; RT is evaluated every call.
 * Change configuration through rt_key_configure(), not direct struct writes.
 * Normalization clamps to 0..RT_TRAVEL_MAX. Linear mode is the nearest
 * 1/RT_TRAVEL_MAX ADC fraction. Curve mode maps normalized ADC through a fixed
 * monotonic reference table before RT, with the same map in both directions.
 * At travel <= top_deadzone: always release and require initial actuation.
 * Initial press: travel >= actuation. During an RT cycle, release after an
 * upward movement >= rt_release from the pressed peak, and repress after a
 * downward movement >= rt_press from the released trough. Partial releases
 * retain that cycle until the top deadzone is reached.
 * With a nonzero bottom deadzone, travel >= MAX-bottom_deadzone holds pressed
 * and anchors the RT peak at that boundary. Release requires rt_release travel
 * above the boundary, so bottom jitter and crossing the boundary alone cannot
 * release/repress. Reported travel snaps to0 at/below top_deadzone and to MAX
 * at/above MAX-bottom_deadzone. The middle is not rescaled. Internal RT travel
 * and extrema retain the selected curve's normalized sample value, so this
 * reporting rule does not change presses/releases/retrigger decisions.
 * With RT off, press at/above actuation and release below actuation.
 * Disabled keys use the same reported endpoint rule, but are always released.
 * changed describes only this call's pressed-state transition.
 * Optional retrigger_before_release (default false): while held outside the
 * bottom deadzone, an upward movement starts a valley. A subsequent downward
 * movement >=rt_press emits retriggered without changing pressed/changed or
 * the normal release peak. Each event clears the valley; monotonic downward
 * motion cannot emit repeatedly. Real release, RT off, reset, and either
 * deadzone clear the valley. Consumer code must serialize actual HID edges. */
rt_result_t rt_key_process(rt_key_t *key, uint16_t adc_raw);

/* Private fractional acquisition path. adc_q5 is in the same ADC endpoint
 * units as above, multiplied by32; endpoints and thresholds do not migrate.
 * The physical 14-bit/eight-scan sum is bounded at16383*8. An invalid input
 * releases and resets the RT cycle, without altering the configuration.
 * Whole ADC-code inputs retain the legacy map exactly. Fractional input
 * precision is not a claim of improved physical sensor resolution. */
rt_result_t rt_key_process_q5(rt_key_t *key, uint32_t adc_q5);

#endif
