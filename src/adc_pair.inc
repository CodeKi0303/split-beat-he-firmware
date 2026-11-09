/* App-only ADC setting transaction. Use one existing C7 relay exchange
 * while output is off. Leave normal S7 packet format and timing unchanged. */
static struct {
    uint8_t query[64];
    uint32_t started;
    bool pending,sent,uncertain;
} adc_pair;

static void adc_pair_result(uint8_t status)
{
    memset(response,0,64);
    memcpy(response,adc_pair.query,6U);response[3]|=0x80U;
    response[6]=status;
    if(!status){response[7]=5U;memcpy(response+8,adc_pair.query+8,5U);}
    update_frame_finish(response);response_pending=1U;
}
static void adc_pair_end(void)
{
    adc_pair.pending=false;
    split_update_mode(false);
}
static bool adc_pair_request(const uint8_t q[64],uint32_t now)
{
    if(q[0]!='C'||q[1]!='7'||q[3]!=ADC_PAIR_COMMAND)return false;
    memcpy(adc_pair.query,q,64U);
    uint32_t hz;
    uint8_t filter;
    if(!update_frame_valid(q)||q[6]||q[7]!=5U)adc_pair_result(C77_BAD_PACKET);
    else if(!adc_scan_parse_tuning(q+8,&hz,&filter))adc_pair_result(C77_BAD_CONFIG);
    else if(engine.output||engine.restore_output_pending||engine.cal_state||engine.save_requested)
        adc_pair_result(C77_BUSY);
    else if(!c77_source_fresh(&engine,1,now))adc_pair_result(C77_NOT_READY);
    else{
        c77_disable(&engine);
        engine.seen[1]=engine.sample[1].valid=0U;
        adc_pair.started=now;adc_pair.pending=true;adc_pair.sent=false;
        split_update_mode(true);
    }
    return true;
}
static void adc_pair_poll(uint32_t now)
{
    if(!adc_pair.pending)return;
    if((uint32_t)(now-adc_pair.started)>=500000U){
        adc_pair.uncertain|=adc_pair.sent;
        adc_pair_end();adc_pair_result(C77_NOT_READY);return;
    }
    if(!adc_pair.sent){
        adc_pair.sent=split_update_send(adc_pair.query);
        return;
    }
    uint8_t reply[64];
    if(!split_update_read(reply)||!update_frame_valid(reply)||reply[3]!=(ADC_PAIR_COMMAND|0x80U)||
       reply[4]!=adc_pair.query[4]||reply[5]!=adc_pair.query[5])return;
    uint8_t status=reply[6];
    if(!status){
        if(reply[7]!=5U||memcmp(reply+8,adc_pair.query+8,5U))status=C77_NOT_READY;
        else{
            uint8_t local[64];
            memcpy(local,adc_pair.query,sizeof local);local[3]=ADC_TUNING_COMMAND;update_frame_finish(local);
            (void)adc_tuning_command(&engine,local,reply,now);
            status=reply[6];
        }
    }
    adc_pair.uncertain=status!=0U;
    adc_pair_end();adc_pair_result(status);
}
static void adc_pair_cancel(void)
{
    if(!adc_pair.pending)return;
    adc_pair.uncertain|=adc_pair.sent;
    adc_pair_end();
}
