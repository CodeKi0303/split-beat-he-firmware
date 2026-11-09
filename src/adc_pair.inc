/* App-only ADC setting transaction. Use one existing C7 relay exchange
 * while output is off. Leave normal S7 packet format and timing unchanged. */
static struct {
    uint8_t query[64];
    uint32_t started;
    bool pending,sent,uncertain,right_was_fresh;
} adc_pair;

static void adc_pair_watch_link(uint32_t now)
{
    if(adc_pair.pending)return;
    bool fresh=c77_source_fresh(&engine,1,now);
    if(adc_pair.right_was_fresh&&!fresh){
        adc_pair.uncertain=true;
        engine.restore_output_pending=false;
        c77_disable(&engine);
    }
    adc_pair.right_was_fresh=fresh;
}

static void adc_pair_result(uint8_t status,const uint8_t *values)
{
    memset(response,0,64);
    memcpy(response,adc_pair.query,6U);response[3]|=0x80U;
    response[6]=status;
    if(!status){response[7]=5U;memcpy(response+8,values,5U);}
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
    if(!update_frame_valid(q)||q[6]||(q[7]!=0U&&q[7]!=5U))adc_pair_result(C77_BAD_PACKET,NULL);
    else if(q[7]&&!adc_scan_parse_tuning(q+8,&hz,&filter))adc_pair_result(C77_BAD_CONFIG,NULL);
    else if(engine.output||engine.restore_output_pending||engine.cal_state||engine.save_requested)
        adc_pair_result(C77_BUSY,NULL);
    else if(!c77_source_fresh(&engine,1,now))adc_pair_result(C77_NOT_READY,NULL);
    else{
        if(q[7])c77_disable(&engine);
        engine.seen[1]=engine.sample[1].valid=0U;
        adc_pair.right_was_fresh=false; /* Intentional S7 pause. */
        adc_pair.started=now;adc_pair.pending=true;adc_pair.sent=false;
        split_update_mode(true);
    }
    return true;
}
static void adc_pair_poll(uint32_t now)
{
    if(!adc_pair.pending)return;
    if((uint32_t)(now-adc_pair.started)>=500000U){
        adc_pair.uncertain|=adc_pair.sent&&adc_pair.query[7];
        adc_pair_end();adc_pair_result(C77_NOT_READY,NULL);return;
    }
    if(!adc_pair.sent){
        adc_pair.sent=split_update_send(adc_pair.query);
        return;
    }
    uint8_t reply[64];
    if(!split_update_read(reply)||!update_frame_valid(reply)||reply[3]!=(ADC_PAIR_COMMAND|0x80U)||
       reply[4]!=adc_pair.query[4]||reply[5]!=adc_pair.query[5])return;
    uint8_t status=reply[6];
    if(!adc_pair.query[7]){
        uint32_t hz;
        uint8_t filter;
        if(!status&&(reply[7]!=5U||!adc_scan_parse_tuning(reply+8,&hz,&filter)))status=C77_NOT_READY;
        adc_pair_end();adc_pair_result(status,reply+8);return;
    }
    if(!status){
        if(reply[7]!=5U||memcmp(reply+8,adc_pair.query+8,5U))status=C77_NOT_READY;
        else{
            uint8_t local[64];
            memcpy(local,adc_pair.query,sizeof local);local[3]=ADC_TUNING_COMMAND;update_frame_finish(local);
            (void)adc_tuning_command(&engine,local,reply,now,true);
            status=reply[6];
        }
    }
    adc_pair.uncertain=status!=0U;
    adc_pair_end();adc_pair_result(status,adc_pair.query+8);
}
static void adc_pair_cancel(void)
{
    if(!adc_pair.pending)return;
    adc_pair.uncertain|=adc_pair.sent&&adc_pair.query[7];
    adc_pair_end();
}
