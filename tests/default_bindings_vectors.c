/* Emit the real C factory configuration for canonical-byte comparison. */
#include "keyboard.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    c77_config cfg;c77_defaults(&cfg);assert(c77_config_valid(&cfg));
    uint8_t blob[C77_CONFIG_BYTES];c77_config_encode(&cfg,blob);
    for(unsigned i=0;i<sizeof blob;i++)printf("%02x",blob[i]);
    puts("");
    return 0;
}
