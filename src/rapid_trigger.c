#include "rapid_trigger.h"
#include "cpu_profile.h"

#include <stddef.h>

enum { RT_CACHE_NONE=0, RT_CACHE_WHOLE=1, RT_CACHE_Q5=2 };

static bool config_valid(const rt_config_t *config)
{
    int32_t span = (int32_t)config->adc_pressed - config->adc_released;
    if (span < 0) span = -span;
    return span >= RT_MIN_ADC_SPAN &&
           config->actuation > config->top_deadzone &&
           config->actuation <= RT_TRAVEL_MAX &&
           config->bottom_deadzone <= RT_TRAVEL_MAX - config->actuation &&
           config->curve_id <= RT_CURVE_OWLAB_TI_REFERENCE &&
           config->rt_press >= 1U && config->rt_press <= RT_TRAVEL_MAX &&
           config->rt_release >= 1U && config->rt_release <= RT_TRAVEL_MAX;
}

static uint16_t normalize_distance(const rt_config_t *config,
                                   int32_t distance,int32_t span)
{
    if (span < 0) {
        span = -span;
        distance = -distance;
    }
    if (distance <= 0) return 0U;
    if (distance >= span) return RT_TRAVEL_MAX;
    /* Keep legacy behavior bit-for-bit. The largest public uint16 endpoint
     * span scaled by32 is2097120, so distance*1000 plus half-span still fits
     * int32_t after clamping. A normalized signal has no distance units. */
    if (config->curve_id == RT_CURVE_LINEAR)
        return (uint16_t)((distance * RT_TRAVEL_MAX + span / 2) / span);

    /* Public NeonHE Tab60 OW Ti reference nodes, not a C86 characterization.
     * Source: https://neonhe.com/assets/index-C2gKFU8p.js (2026-11-07).
     * Its original 3.3 mm axis is normalized here; host total travel 3.4 mm
     * proportionally scales it. Other strokes reuse the same approximate
     * shape. Calibration endpoints and ADC polarity remain per-key.
     * Round signal, interpolated axis, then output using integer arithmetic;
     * Python/JS implement this exact sequence. No direction-dependent map. */
    static const uint16_t signal_nodes[] = {0,467,1211,2244,3667,5644,8500,10000};
    static const uint16_t axis_nodes[] = {0,5000,10000,15000,20000,25000,30000,33000};
    /* Real12-bit endpoints keep distance <=131064 and use hardware32-bit
     * division. The generic RT API also accepts full uint16 endpoints;
     * inverted Q5 spans outside physical ADC range need a64-bit numerator. */
    const uint32_t signal = (uint32_t)distance<=RT_ADC_Q5_MAX ?
        ((uint32_t)distance*10000U+(uint32_t)span/2U)/(uint32_t)span :
        (uint32_t)(((uint64_t)(uint32_t)distance*10000U+
                    (uint32_t)span/2U)/(uint32_t)span);
    unsigned upper = 1;
    while (upper < 7 && signal > signal_nodes[upper]) upper++;
    const uint32_t width = signal_nodes[upper] - signal_nodes[upper - 1];
    const uint32_t rise = axis_nodes[upper] - axis_nodes[upper - 1];
    const uint32_t axis = axis_nodes[upper - 1] +
        ((signal - signal_nodes[upper - 1]) * rise + width / 2) / width;
    return (uint16_t)((axis * RT_TRAVEL_MAX + 16500) / 33000);
}

static uint16_t normalize(const rt_config_t *config, uint16_t adc_raw)
{
    return normalize_distance(config,
        (int32_t)adc_raw-config->adc_released,
        (int32_t)config->adc_pressed-config->adc_released);
}

void rt_config_defaults(rt_config_t *config, uint16_t adc_released,
                        uint16_t adc_pressed)
{
    if (config == NULL) return;
    config->adc_released = adc_released;
    config->adc_pressed = adc_pressed;
    config->actuation = 200U;
    config->rt_press = 50U;
    config->rt_release = 50U;
    config->top_deadzone = 20U;
    config->bottom_deadzone = 0U;
    config->curve_id = RT_CURVE_LINEAR;
    config->enabled = true;
    config->rapid_trigger_enabled = true;
    config->retrigger_before_release = false;
}

void rt_key_reset(rt_key_t *key)
{
    if (key == NULL) return;
    key->travel = 0U;
    key->extreme = 0U;
    key->pressed = false;
    key->rapid_cycle = false;
    key->retrigger_armed = false;
    key->retrigger_valley = 0U;
    key->normalization_kind = RT_CACHE_NONE;
}

bool rt_key_configure(rt_key_t *key, const rt_config_t *config)
{
    if (key == NULL) return false;
    rt_key_reset(key);
    key->valid = false;
    if (config == NULL) return false;
    key->config = *config;
    key->valid = config_valid(config);
    return key->valid;
}

static rt_result_t process_travel(rt_key_t *key, uint16_t travel)
{
    rt_result_t result = {0U, false, false, false};
    bool was_pressed;
    if (key == NULL || !key->valid) return result;
    was_pressed = key->pressed;
    const uint16_t previous_travel = key->travel;
    key->travel = travel;

    if (!key->config.enabled || key->travel <= key->config.top_deadzone) {
        key->pressed = false;
        key->rapid_cycle = false;
        key->extreme = key->travel;
    } else if (!key->config.rapid_trigger_enabled) {
        key->pressed = key->travel >= key->config.actuation;
        key->rapid_cycle = false;
        key->extreme = key->travel;
    } else if (key->config.bottom_deadzone &&
               key->travel >= RT_TRAVEL_MAX - key->config.bottom_deadzone) {
        key->pressed = true;
        key->rapid_cycle = true;
        key->extreme = RT_TRAVEL_MAX - key->config.bottom_deadzone;
    } else if (!key->rapid_cycle) {
        if (key->travel >= key->config.actuation) {
            key->pressed = true;
            key->rapid_cycle = true;
            key->extreme = key->travel;
        }
    } else if (key->pressed) {
        if (key->travel > key->extreme) key->extreme = key->travel;
        if ((uint16_t)(key->extreme - key->travel) >= key->config.rt_release) {
            key->pressed = false;
            key->extreme = key->travel;
        }
    } else {
        if (key->travel < key->extreme) key->extreme = key->travel;
        if ((uint16_t)(key->travel - key->extreme) >= key->config.rt_press) {
            key->pressed = true;
            key->extreme = key->travel;
        }
    }

    /* Keep the ordinary release peak unchanged. This optional path adds an
     * event, not a new physical pressed state or a changed release threshold. */
    if (!was_pressed || !key->pressed || !key->config.rapid_trigger_enabled ||
        !key->config.retrigger_before_release ||
        (key->config.bottom_deadzone &&
         travel >= RT_TRAVEL_MAX-key->config.bottom_deadzone)) {
        key->retrigger_armed = false;
    } else {
        if (!key->retrigger_armed && travel < previous_travel) {
            key->retrigger_armed = true;
            key->retrigger_valley = travel;
        }
        if (key->retrigger_armed) {
            if (travel < key->retrigger_valley) key->retrigger_valley = travel;
            if ((uint16_t)(travel-key->retrigger_valley) >= key->config.rt_press) {
                result.retriggered = true;
                key->retrigger_armed = false;
            }
        }
    }
    /* Report settled endpoints without moving RT's peak/valley or previous
     * sample. No rescaling: the middle retains the normalized curve value. */
    result.travel = key->travel <= key->config.top_deadzone ? 0U :
        key->travel >= RT_TRAVEL_MAX-key->config.bottom_deadzone ?
            RT_TRAVEL_MAX : key->travel;
    result.pressed = key->pressed;
    result.changed = key->pressed != was_pressed;
    return result;
}

rt_result_t rt_key_process(rt_key_t *key, uint16_t adc_raw)
{
    const rt_result_t invalid={0U,false,false,false};
    if(key==NULL || !key->valid) return invalid;
    CPU_PROFILE_RT_CALL();
    if(key->normalization_kind!=RT_CACHE_WHOLE || key->normalization_input!=adc_raw) {
        CPU_PROFILE_BEGIN(prof_normalize);
        key->normalization_travel=normalize(&key->config,adc_raw);
        CPU_PROFILE_END(prof_normalize,key->config.curve_id==RT_CURVE_LINEAR?
                        CPU_PROF_LINEAR_MISS:CPU_PROF_CURVE_MISS);
        key->normalization_input=adc_raw;key->normalization_kind=RT_CACHE_WHOLE;
    }
    return process_travel(key,key->normalization_travel);
}

rt_result_t rt_key_process_q5(rt_key_t *key, uint32_t adc_q5)
{
    rt_result_t invalid={0U,false,false,false};
    if(key==NULL || !key->valid) return invalid;
    if(adc_q5>RT_ADC_Q5_MAX) {
        invalid.changed=key->pressed;
        rt_key_reset(key);
        return invalid;
    }
    CPU_PROFILE_RT_CALL();
    if(key->normalization_kind!=RT_CACHE_Q5 || key->normalization_input!=adc_q5) {
        const int32_t distance=(int32_t)adc_q5-
            (int32_t)key->config.adc_released*RT_ADC_Q5_SCALE;
        const int32_t span=((int32_t)key->config.adc_pressed-
            key->config.adc_released)*RT_ADC_Q5_SCALE;
        CPU_PROFILE_BEGIN(prof_normalize);
        key->normalization_travel=normalize_distance(&key->config,distance,span);
        CPU_PROFILE_END(prof_normalize,key->config.curve_id==RT_CURVE_LINEAR?
                        CPU_PROF_LINEAR_MISS:CPU_PROF_CURVE_MISS);
        key->normalization_input=adc_q5;key->normalization_kind=RT_CACHE_Q5;
    }
    return process_travel(key,key->normalization_travel);
}
