#include "keyboard.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    c77_config config;
    c77_defaults(&config);
    for(unsigned key=0;key<C77_HALL;key++){
        config.keys[key].released=3000U;
        config.keys[key].pressed=1000U;
        config.keys[key].flags=7U;
    }
    c77_engine engine;
    c77_engine_init(&engine,&config,6U,true);
    c77_sample_t sample={0};
    sample.valid=1U;
    sample.hall_filtered_valid=1U;
    for(unsigned key=0;key<7U;key++){
        sample.raw[key]=3000U;
        sample.hall_filtered[key]=3000U;
    }

    sample.sequence=40U;
    sample.timestamp_us=1000U;
    sample.raw[0]=2450U;
    sample.hall_filtered[0]=2400U;
    sample.raw[4]=2300U;
    sample.hall_filtered[4]=2200U;
    assert(c77_update_left_bank(&engine,0U,&sample,1000U));
    assert(engine.travel[0]==300U&&engine.travel[4]==0U);
    assert(engine.sample[0].raw[0]==2450U&&!c77_source_fresh(&engine,0U,1000U));

    sample.sequence=19U;
    sample.timestamp_us=1100U;
    sample.digital=7U;
    assert(c77_update_left_bank(&engine,1U,&sample,1100U));
    assert(engine.travel[0]==300U&&engine.travel[4]==400U);
    assert(engine.sample[0].digital==0U);
    assert(engine.sample[0].timestamp_us==1000U&&engine.sample[0].sequence==2U);
    assert(c77_source_fresh(&engine,0U,1100U));
    assert(!c77_update_left_bank(&engine,1U,&sample,1101U));
    assert(engine.sample[0].sequence==2U);

    engine.cal_key=4U;
    engine.cal_state=1U;
    sample.sequence=41U;
    sample.timestamp_us=1500U;
    sample.hall_filtered[0]=3000U;
    assert(c77_update_left_bank(&engine,0U,&sample,1500U));
    assert(engine.cal_count==0U&&engine.travel[0]==0U&&engine.travel[4]==400U);
    sample.sequence=20U;
    assert(c77_update_left_bank(&engine,1U,&sample,1500U));
    assert(engine.cal_count==1U);
    assert(!c77_update_left_bank(&engine,1U,&sample,1501U)&&engine.cal_count==1U);

    sample.sequence=42U;
    sample.timestamp_us=2450U;
    assert(c77_update_left_bank(&engine,0U,&sample,2450U));
    assert(c77_source_fresh(&engine,0U,2450U));
    engine.output=1U;
    c77_tick(&engine,2501U);
    assert(!engine.output&&!engine.pressed_mask&&!c77_source_fresh(&engine,0U,2501U));
    sample.sequence=43U;
    sample.timestamp_us=2550U;
    assert(c77_update_left_bank(&engine,0U,&sample,2550U));
    assert(!c77_source_fresh(&engine,0U,2550U));

    sample.sequence=3U;
    assert(c77_update_left_bank(&engine,1U,&sample,2550U));
    assert(!engine.cal_state&&engine.cal_key==255U&&c77_source_fresh(&engine,0U,2550U));

    c77_engine_init(&engine,&config,6U,true);
    sample.sequence=UINT32_MAX;
    sample.timestamp_us=UINT32_MAX-40U;
    assert(c77_update_left_bank(&engine,0U,&sample,sample.timestamp_us));
    sample.sequence=37U;
    sample.timestamp_us=UINT32_MAX-20U;
    assert(c77_update_left_bank(&engine,1U,&sample,sample.timestamp_us));
    engine.output=1U;
    sample.sequence=0U;
    sample.timestamp_us=25U;
    assert(c77_update_left_bank(&engine,0U,&sample,25U));
    assert(engine.output&&c77_source_fresh(&engine,0U,25U));
    assert(engine.sample[0].timestamp_us==UINT32_MAX-20U);
    puts("Async Hall banks: independent sequence/RT, calibration, digital ownership, wrap and stale release PASS");
    return 0;
}
