/* Shared contract checks run against the real single/dual publication path
 * and the production accessor extracted by run_adc_tests.py. */
static uint16_t trace_test_raw(unsigned frame,unsigned key)
{
    return (uint16_t)(1000U+key*50U+(frame*37U+key*11U)%300U);
}
static void trace_test_emit(unsigned frame);
static void test_trace_snapshot(void)
{
    c77_sample_t sample={0};adc_trace_history history;
    const uint8_t keys[ADC_TRACE_KEYS]={5U,6U};
    memset(&history,0xa5,sizeof history);
    const adc_trace_history untouched=history;
    reset();
    basepri=0x30U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    assert(memcmp(&untouched,&history,sizeof history)==0);
    for(unsigned frame=0;frame<32U;frame++) {
        trace_test_emit(frame);
        const uint32_t old_mask=frame%3U==0U?0U:frame%3U==1U?0x30U:8U;
        basepri=old_mask;
        const bool valid=adc_scan_trace_snapshot(&sample,&history,keys);
        assert(basepri==old_mask);
        if(frame<7U) {
            assert(!valid && memcmp(&untouched,&history,sizeof history)==0);
            continue;
        }
        assert(valid && sample.sequence==frame+1U);
        assert(sample.timestamp_us==clock_us && sample.hall_filtered_valid);
        for(unsigned k=0;k<ADC_TRACE_KEYS;k++) {
            unsigned sum=0U;
            for(unsigned i=0;i<ADC_TRACE_HISTORY;i++) {
                const uint16_t expected=trace_test_raw(frame-7U+i,keys[k]);
                assert(history.raw[k][i]==expected);
                sum+=expected;
            }
            assert(sample.raw[keys[k]]==history.raw[k][7]);
            assert(sample.hall_filtered[keys[k]]==(sum+4U)/8U);
        }
    }
    /* Reverse the selected channels: response order follows the request. */
    const uint8_t reverse[ADC_TRACE_KEYS]={6U,5U};
    assert(adc_scan_trace_snapshot(&sample,&history,reverse));
    assert(history.raw[0][7]==trace_test_raw(31U,6U));
    assert(history.raw[1][0]==trace_test_raw(24U,5U));
    const uint8_t duplicate[ADC_TRACE_KEYS]={5U,5U},outside[ADC_TRACE_KEYS]={7U,0U};
    basepri=0x30U;
    assert(!adc_scan_trace_snapshot(&sample,&history,duplicate) && basepri==0x30U);
    assert(!adc_scan_trace_snapshot(&sample,&history,outside) && basepri==0x30U);
    const uint32_t captured_at=clock_us;
    clock_us=captured_at+1000U;
    assert(adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    clock_us=captured_at+1001U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    clock_us=captured_at;
    scan_state.errors=1U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    scan_state.errors=0U;
    local_hall_filter.next=8U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    local_hall_filter.next=0U;local_hall_filter.ready=0U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    local_hall_filter.ready=1U;input_frames[published_frame].hall_filtered_valid=0U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    input_frames[published_frame].hall_filtered_valid=1U;scan_state.running=0U;
    assert(!adc_scan_trace_snapshot(&sample,&history,keys) && basepri==0x30U);
    reset();trace_test_emit(0U);
    assert(!adc_scan_trace_snapshot(&sample,&history,keys));
    printf("ADC trace: startup, rolling8 order/rounded mean, selected keys, freshness and mask restore PASS\n");
}
