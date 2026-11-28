/* Offline vectors from actual RT core. No device or OS input access. */
#include "rapid_trigger.h"
#include <stdio.h>

int main(void)
{
    const uint16_t endpoints[][2] = {
        {0, 4095}, {4095, 0}, {2206, 2634}, {2634, 2206},
        {2000, 2128}, {2128, 2000}
    };
    /* A gentle-top drawn table (curve 2) plus the identity table. */
    static const uint16_t drawn[RT_CUSTOM_CURVE_POINTS] = {
        150, 350, 650, 1050, 1550, 2150, 2850, 3650, 4550, 5500, 6450, 7400, 8300, 9100, 9700
    };
    static const uint16_t curves[] = {RT_CURVE_LINEAR, RT_CURVE_OWLAB_TI_REFERENCE,
        RT_CURVE_CUSTOM, RT_CURVE_CUSTOM, RT_CURVE_WISH_OWLAB_TI};
    puts("[");
    for (unsigned e = 0; e < sizeof endpoints / sizeof endpoints[0]; ++e) {
        for (unsigned curve = 0; curve < sizeof curves / sizeof curves[0]; ++curve) {
            rt_config_t config;
            rt_key_t key;
            rt_config_defaults(&config, endpoints[e][0], endpoints[e][1]);
            config.curve_id = curves[curve];
            if (curve == 3) for (unsigned i = 0; i < RT_CUSTOM_CURVE_POINTS; ++i) config.custom_curve[i] = drawn[i];
            config.enabled = false;
            if (!rt_key_configure(&key, &config)) return 2;
            if (e || curve) puts(",");
            printf("{\"released\":%u,\"pressed\":%u,\"curve_id\":%u,\"custom_curve\":[",
                   (unsigned)endpoints[e][0], (unsigned)endpoints[e][1], (unsigned)config.curve_id);
            for (unsigned i = 0; i < RT_CUSTOM_CURVE_POINTS; ++i)
                printf("%s%u", i ? "," : "", (unsigned)config.custom_curve[i]);
            printf("],\"travel\":[");
            for (unsigned raw = 0; raw <= 4095; ++raw) {
                rt_result_t result = rt_key_process(&key, (uint16_t)raw);
                if (result.pressed) return 3;
                /* Pure curve parity excludes the separate reported deadzone clamp. */
                printf("%s%u", raw ? "," : "", (unsigned)key.travel);
            }
            printf("]}");
        }
    }
    puts("\n]");
    return 0;
}
