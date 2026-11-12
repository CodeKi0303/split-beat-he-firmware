/* Offline vectors from actual RT core. No device or OS input access. */
#include "rapid_trigger.h"
#include <stdio.h>

int main(void)
{
    const uint16_t endpoints[][2] = {
        {0, 4095}, {4095, 0}, {2206, 2634}, {2634, 2206},
        {2000, 2128}, {2128, 2000}
    };
    puts("[");
    for (unsigned e = 0; e < sizeof endpoints / sizeof endpoints[0]; ++e) {
        for (unsigned curve = 0; curve < 2; ++curve) {
            rt_config_t config;
            rt_key_t key;
            rt_config_defaults(&config, endpoints[e][0], endpoints[e][1]);
            config.curve_id = (uint16_t)curve;
            config.enabled = false;
            if (!rt_key_configure(&key, &config)) return 2;
            if (e || curve) puts(",");
            printf("{\"released\":%u,\"pressed\":%u,\"curve_id\":%u,\"travel\":[",
                   (unsigned)endpoints[e][0], (unsigned)endpoints[e][1], curve);
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
