#ifndef RAPID_TRIGGER_H
#define RAPID_TRIGGER_H

#include <stdbool.h>
#include <stdint.h>

/* Normalized travel. Curve 0 is ADC range; curve 1 is an approximate
 * reference distance fraction, not independently measured physical distance. */
enum {
    RT_TRAVEL_MAX = 1000,
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
} rt_config_t;

typedef struct {
    rt_config_t config;
    uint16_t travel;
    uint16_t extreme;
    bool pressed;
    bool rapid_cycle;
    bool valid;
    /* Private normalization cache, never serialized. Configure/reset invalidate
     * it; a cache hit still runs the complete RT state machine. */
    uint8_t normalization_kind;
    uint32_t normalization_input;
    uint16_t normalization_travel;
} rt_key_t;

typedef struct {
    uint16_t travel;
    bool pressed;
    bool changed;
} rt_result_t;

/* Defaults: initial actuation 200; RT press/release 50; top deadzone 20;
 * bottom deadzone 0 (disabled), curve 0 (linear, legacy-compatible).
 * Both key processing and rapid trigger are enabled. ADC polarity may be
 * either direction. Endpoints must be measured by the caller. */
void rt_config_defaults(rt_config_t *config, uint16_t adc_released,
                        uint16_t adc_pressed);

/* Configure only outside concurrent processing (e.g. pause the sample IRQ).
 * Every call releases the key and resets the initial-actuation state.
 * Invalid configuration disables processing and returns false. Required:
 * abs(ADC span) >= 128; top_deadzone < actuation <= 1000;
 * 0 <= bottom_deadzone <= 1000 - actuation;
 * curve_id is RT_CURVE_LINEAR or RT_CURVE_OWLAB_TI_REFERENCE;
 * 1 <= rt_press, rt_release <= 1000. */
bool rt_key_configure(rt_key_t *key, const rt_config_t *config);

/* Preserve configuration while releasing/resetting the key. The caller must
 * publish the new released state; reset/configure do not return an edge. */
void rt_key_reset(rt_key_t *key);

/* Call once per sample. No filtering, heap, floating point, or HAL calls.
 * Identical ADC input reuses normalization only; RT is evaluated every call.
 * Change configuration through rt_key_configure(), not direct struct writes.
 * Normalization clamps to 0..1000. Linear mode retains the original nearest
 * 1/1000 ADC conversion. Curve mode maps normalized ADC through a fixed
 * monotonic reference table before RT, with the same map in both directions.
 * At travel <= top_deadzone: always release and require initial actuation.
 * Initial press: travel >= actuation. During an RT cycle, release after an
 * upward movement >= rt_release from the pressed peak, and repress after a
 * downward movement >= rt_press from the released trough. Partial releases
 * retain that cycle until the top deadzone is reached.
 * With a nonzero bottom deadzone, travel >= 1000-bottom_deadzone holds pressed
 * and anchors the RT peak at that boundary. Release requires rt_release travel
 * above the boundary, so bottom jitter and crossing the boundary alone cannot
 * release/repress. Reported travel remains the selected curve's normalized
 * sample value, not a remapping of the deadzone or a measurement-accuracy claim.
 * With RT off, press at/above actuation and release below actuation.
 * Disabled keys still report normalized travel, but are always released.
 * changed describes only this call's pressed-state transition. */
rt_result_t rt_key_process(rt_key_t *key, uint16_t adc_raw);

/* Private fractional acquisition path. adc_q5 is in the same ADC endpoint
 * units as above, multiplied by32; endpoints and thresholds do not migrate.
 * The physical 14-bit/eight-scan sum is bounded at16383*8. An invalid input
 * releases and resets the RT cycle, without altering the configuration.
 * Whole ADC-code inputs retain the legacy map exactly. Fractional input
 * precision is not a claim of improved physical sensor resolution. */
rt_result_t rt_key_process_q5(rt_key_t *key, uint32_t adc_q5);

#endif
