#include "target_prepare.h"
tp_result_t target_prepare(target_engine_t *e,const batt_profile_t *p,int soc) {
    te_init(e,0);
    if(!p || (p->status!=PROF_BENCH_TEST && p->status!=PROF_VERIFIED)) return TP_BAD_PROFILE;
    if(p->i2c_addr<8 || p->i2c_addr>0x77) return TP_BAD_ADDRESS;
    if(p->n_regs && !p->regs) return TP_NO_RESPONSE;
    for(unsigned k=0;k<p->n_regs;k++) {
        const reg_block_t *b=&p->regs[k];
        if(!b->data || !b->len || (unsigned)b->reg+b->len>256) return TP_NO_RESPONSE;
    }
    uint8_t img[TE_SNAP_MAX],base=0,len=0;
    if(p->soc_build) {
        if(!p->soc_build(soc,img,&base,&len) || !len || len>TE_SNAP_MAX || (unsigned)base+len>256) return TP_BUILD_FAILED;
    } else if(!p->n_regs) return TP_NO_RESPONSE;
    te_init(e,p);
    if(p->soc_build && !te_publish(e,base,img,len)) {te_init(e,0);return TP_BUILD_FAILED;}
    return TP_OK;
}
