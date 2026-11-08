/* Included by the diagnostic left app only. Requests are serviced on the
 * next newly consumed local frame, without extra RT updates or ISR work. */
static struct {
    uint32_t requested_us;
    uint8_t keys[ADC_TRACE_KEYS],transaction[2];
    bool pending;
} adc_trace;

static void trace_put16(uint8_t *p,uint16_t value)
{
    p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);
}
static void trace_put32(uint8_t *p,uint32_t value)
{
    trace_put16(p,(uint16_t)value);trace_put16(p+2,(uint16_t)(value>>16));
}
static void trace_response(uint8_t status,const c77_sample_t *sample,
                           const adc_trace_history *history)
{
    memset(response,0,sizeof response);
    response[0]='C';response[1]='7';response[2]=1;response[3]=ADC_TRACE_COMMAND|0x80U;
    response[4]=adc_trace.transaction[0];response[5]=adc_trace.transaction[1];
    response[6]=status;
    if(status==C77_OK){
        uint8_t *out=response+8;
        response[7]=54;
        trace_put32(out,sample->sequence);trace_put32(out+4,sample->timestamp_us);
        out[8]=adc_trace.keys[0];out[9]=adc_trace.keys[1];
        for(unsigned k=0;k<ADC_TRACE_KEYS;k++){
            const unsigned key=adc_trace.keys[k];
            uint8_t *row=out+10+k*22;
            for(unsigned i=0;i<ADC_TRACE_HISTORY;i++)
                trace_put16(row+i*2,history->raw[k][i]);
            trace_put16(row+16,sample->hall_filtered[key]);
            trace_put16(row+18,engine.filtered[key]);
            trace_put16(row+20,engine.travel[key]|((engine.pressed_mask&(1U<<key))?0x8000U:0U));
        }
    }
    update_frame_finish(response);
    adc_trace.pending=false;response_pending=1;
}
static bool trace_request(const uint8_t request[64],uint32_t now)
{
    if(request[0]!='C' || request[1]!='7' || request[3]!=ADC_TRACE_COMMAND) return false;
    adc_trace.transaction[0]=request[4];adc_trace.transaction[1]=request[5];
    if(!update_frame_valid(request) || !(request[4]|request[5]) || request[6] || request[7]!=2U)
        trace_response(C77_BAD_PACKET,NULL,NULL);
    else if(request[8]>=7U || request[9]>=7U || request[8]==request[9])
        trace_response(C77_BAD_INDEX,NULL,NULL);
    else if(engine.output || engine.dirty || engine.cal_state || !engine.stored_valid)
        trace_response(C77_BUSY,NULL,NULL);
    else {
        adc_trace.keys[0]=request[8];adc_trace.keys[1]=request[9];
        adc_trace.requested_us=now;adc_trace.pending=true;
    }
    return true;
}
