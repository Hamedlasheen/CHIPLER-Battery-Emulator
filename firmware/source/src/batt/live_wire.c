#include "live_wire.h"
#include <string.h>
#define RX_FIRST 0x0800u /* IC_DATA_CMD.FIRST_DATA_BYTE, RP2350 SDK register header */
void live_wire_init(live_wire_t *w,st_target_t *t,uint32_t now) {
    memset(w,0,sizeof *w);w->target=t;w->last_us=now;
}
TE_RAMFUNC static bool fail(live_wire_t *w,st_result_t reason,uint8_t site) {
    w->blocked=true;w->fault=reason;w->site=site;st_abort(w->target);return false;
}
TE_RAMFUNC bool live_wire_event(live_wire_t *w,const live_event_t *e,uint32_t now,live_reply_t *reply) {
    reply->have_tx=false;
    if(w->blocked)return false;
    st_target_t *t=w->target;w->last_us=now;
    if(e->error || e->count>16)return fail(w,ST_ABORTED,1);
    unsigned first=0;for(unsigned k=0;k<e->count;k++)first+=(e->rx[k]&RX_FIRST)!=0;
    /* Coalesced boundaries whose ordering cannot be reconstructed are rejected, not guessed. */
    if(first>1 || (e->stop&&e->request) || (e->count&&t->reading&&e->stop) ||
       (e->stop&&first&&t->active))return fail(w,ST_ABORTED,2);
    /* DEEPSEEK P02 (drop "&&t->active") NOT ADOPTED: an idle engine + [FIRST byte][STOP] in one batch is also the
     * legitimate command-only STOP of a late ISR (test_live_smbus). If it was really [STOP][new START+command],
     * the swallowed command makes the next RD_REQ find an idle engine and isolate (fail closed, never a made-up
     * reply) - tests/host/test_live_port_mmio.c T7b. See CLAUDE_REVIEW_RESPONSE.md. */
    for(unsigned k=0;k<e->count;k++){
        if(e->rx[k]&RX_FIRST){if(t->active)st_on_restart_write(t);}
        else if(!t->active)return fail(w,ST_ABORTED,3); /* missed START/first byte */
        if(!st_on_rx(t,(uint8_t)e->rx[k]))return fail(w,t->last,4);
    }
    if(e->nack)st_on_nack(t);
    if(e->request){
        if(!t->reading&&!t->active&&!e->count)(void)st_resume_pointer(t); /* known retained pointer only (R3) */
        if(!t->reading&&!st_begin_read(t))return fail(w,t->last,5);
        if(!st_on_tx(t,&reply->tx))return fail(w,t->last,6);
        reply->have_tx=true;
    }
    if(e->stop&&t->active){
        st_result_t r=st_on_stop(t);
        if(r!=ST_OK&&r!=ST_SHORT_A8)return fail(w,r,7);
    }
    return true;
}
TE_RAMFUNC bool live_wire_expire(live_wire_t *w,uint32_t now,uint32_t idle) {
    /* Signed age: the caller samples `now` before taking the guard, so an IRQ can stamp last_us AFTER now.
     * An unsigned (now - last_us) then wraps to ~4e9 us and isolated the port in the middle of a live read
     * (owner bench 2026-10-27, irq-hist site=8 with RD_REQ every byte). A negative age is simply "not idle". */
    if(w->blocked || !w->target->active || (int32_t)(now-w->last_us)<=(int32_t)idle)return false;
    fail(w,ST_ABORTED,8);return true;
}
