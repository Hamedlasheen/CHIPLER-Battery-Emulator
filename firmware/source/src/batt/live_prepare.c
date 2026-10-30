#include "live_prepare.h"
#include "ds12p_config.h"
#include <string.h>
typedef struct {uint8_t cmd;st_shape_t shape;uint8_t count,data[32];} captured_response_t;
typedef struct {uint8_t cmd,len;} captured_write_shape_t;
#include "captured_responses.inc"
#include "phone_test_responses.inc"   /* ported from beta2: T06 single-session samples (experimental, data only) */
#if CHIPLER_12P_REFERENCE
#include "ds12p_evidence.h"
#include "ds12p_evidence_power.inc"   /* 12Pro no-charger reference, owner capture 2026-10-23 */
#include "ds12p_evidence_charger.inc" /* 12Pro charger-connected reference, owner capture 2026-10-23 */
#include "ds12p_writes.inc"
static bool install_evidence_reads(st_target_t *t,const ds_ev_row_t *rows,unsigned count){
    for(unsigned k=0;k<count;k++){
        const ds_ev_row_t *r=&rows[k];
        /* R2: 0x66/0x74 are served by the selector-page wire hook and must NOT be
         * registered statically, otherwise a read before any selector would leak data. */
        if(r->cmd==0x66||r->cmd==0x74)continue;
        st_shape_t sh=r->shape==DS_EV_WORD?ST_SHAPE_WORD:ST_SHAPE_BLOCK;
        if(!st_add(t,r->cmd,sh,r->count))return false;
        if(r->shape==DS_EV_WORD){if(!st_set_word(t,r->cmd,(uint16_t)(r->data[0]|((uint16_t)r->data[1]<<8))))return false;}
        else if(!st_set_block(t,r->cmd,r->data,r->count))return false;
    }
    return true;
}
#endif
static bool install_reads(st_target_t *t,const captured_response_t *rows,unsigned count){
    for(unsigned k=0;k<count;k++){
        const captured_response_t *r=&rows[k];
        if(!st_add(t,r->cmd,r->shape,r->count))return false;
        if(r->shape==ST_SHAPE_WORD){
            if(!st_set_word(t,r->cmd,(uint16_t)(r->data[0]|((uint16_t)r->data[1]<<8))))return false;
        }else if(!st_set_block(t,r->cmd,r->data,r->count))return false;
    }
    return true;
}
bool live_prepare(st_target_t *t,const batt_profile_t *p,int soc) {
    if(!p || p->i2c_addr!=0x0B)return false;
    bool phone=profile_uses_12pm_backend(p);
    bool bench=p->status==PROF_BENCH_TEST;
    if(!phone&&!bench)return false;
#if CHIPLER_12P_REFERENCE
    int cap=profile_12p_capture_state(p);
    if(cap){
        /* 1 = recorded POWER (no charger) reference; 2 = recorded CHARGER state.
         * Each profile serves exactly one captured state; page targets come from the hook. */
        const ds_ev_row_t *rows = cap==1 ? DS12P_POWER_ROWS : DS12P_CHARGER_ROWS;
        unsigned nrows = cap==1
            ? (unsigned)(sizeof DS12P_POWER_ROWS/sizeof DS12P_POWER_ROWS[0])
            : (unsigned)(sizeof DS12P_CHARGER_ROWS/sizeof DS12P_CHARGER_ROWS[0]);
        if(!install_evidence_reads(t,rows,nrows))return false;
        for(unsigned k=0;k<sizeof DS12P_WRITES/sizeof DS12P_WRITES[0];k++)
            if(!st_add(t,DS12P_WRITES[k].cmd,ST_SHAPE_WRITE,DS12P_WRITES[k].len))return false;
        return st_seal(t);
    }
#endif
    if(phone || !strcmp(p->id,"CAP12PM-B1")){
        if(phone){if(!install_reads(t,phone_test_responses,sizeof phone_test_responses/sizeof phone_test_responses[0]))return false;}
        else if(!install_reads(t,captured_responses,sizeof captured_responses/sizeof captured_responses[0]))return false;
        for(unsigned k=0;k<sizeof captured_writes/sizeof captured_writes[0];k++){
            if(!st_add(t,captured_writes[k].cmd,ST_SHAPE_WRITE,captured_writes[k].len))return false;
        }
    }else if(!strcmp(p->id,"SMB-BENCH")){
        uint8_t img[32],base=0,len=0;
        if(!p->soc_build || !p->soc_build(soc,img,&base,&len) || base!=0x20 || len!=4)return false;
        /* Explicit synthetic block 0x20; never infer commands from flat legacy ranges. */
        if(!st_add(t,base,ST_SHAPE_BLOCK,len)||!st_set_block(t,base,img,len))return false;
    }else return false;
    return st_seal(t);
}
bool live_publish_soc(st_target_t *t,uint8_t cmd,const uint8_t *data,uint8_t len) {
    if(cmd==SOC_PUBLISH_WORDS){   /* phone DATA TEST: {cmd,lo,hi} triplets, WORD registers only, one guarded update */
        if(!t->sealed||t->n_regs<=1||!data||len==0||len%3||len>12)return false;
        uint8_t c[4],n=len/3;uint16_t v[4];
        for(uint8_t k=0;k<n;k++){c[k]=data[3*k];v[k]=(uint16_t)(data[3*k+1]|((uint16_t)data[3*k+2]<<8));}
        return st_set_words(t,n,c,v);
    }
    /* Only synthetic block 0x20 is mutable in beta1; captured phone samples are immutable. */
    return t->sealed&&t->n_regs==1&&cmd==0x20&&len==4&&st_set_block(t,cmd,data,len);
}
