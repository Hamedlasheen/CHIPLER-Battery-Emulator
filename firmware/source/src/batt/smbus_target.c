#include "smbus_target.h"
#include <string.h>
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"IRQ write queue must be lock-free");
/* PEC algorithm and counted read/write framing adapted from DeepSeek smbus_target.c. */
TE_RAMFUNC static uint8_t step(uint8_t c,uint8_t b) {
    c^=b; for(int k=0;k<8;k++)c=(uint8_t)((c<<1)^((c&128u)?7u:0u)); return c;
}
TE_RAMFUNC uint8_t st_pec_read(uint8_t a,uint8_t cmd,const uint8_t *p,size_t n) {
    uint8_t c=step(step(step(0,(uint8_t)(a<<1)),cmd),(uint8_t)((a<<1)|1));
    for(size_t k=0;k<n;k++)c=step(c,p[k]);
    return c;
}
TE_RAMFUNC uint8_t st_pec_write(uint8_t a,uint8_t cmd,const uint8_t *p,size_t n) {
    uint8_t c=step(step(0,(uint8_t)(a<<1)),cmd);
    for(size_t k=0;k<n;k++)c=step(c,p[k]);
    return c;
}
TE_RAMFUNC static uint32_t enter(st_target_t *t) {return t->enter?t->enter(t->guard_ctx):0;}
TE_RAMFUNC static void leave(st_target_t *t,uint32_t x) {if(t->leave)t->leave(t->guard_ctx,x);}
TE_RAMFUNC static st_reg_t *find(st_target_t *t,uint8_t cmd) {
    for(unsigned k=0;k<t->n_regs;k++)if(t->regs[k].cmd==cmd)return &t->regs[k];
    return NULL;
}
TE_RAMFUNC static void reset_txn(st_target_t *t) {
    t->active=t->reading=t->nacked=t->overread=t->invalid=t->idle_fill=false;
    t->nw=t->wire_len=t->tx_off=0; st_zero(t->w,sizeof t->w);
}
void st_init(st_target_t *t,uint8_t a) {
    memset(t,0,sizeof *t);t->addr7=a;atomic_init(&t->write_head,0);atomic_init(&t->write_tail,0);
}
void st_set_hooks(st_target_t *t,const st_hooks_t *h,void *ctx) {
    if(!t)return;
    if(h)t->hooks=*h; else st_zero(&t->hooks,sizeof t->hooks);
    t->hooks_ctx=ctx;
}
bool st_set_guard(st_target_t *t,st_guard_enter_fn a,st_guard_exit_fn b,void *ctx) {
    if(t->sealed || !a || !b)return false;
    t->enter=a;t->leave=b;t->guard_ctx=ctx;return true;
}
bool st_add(st_target_t *t,uint8_t cmd,st_shape_t s,uint8_t n) {
    if(t->sealed || n<1 || n>32 || (s==ST_SHAPE_WORD&&n!=2) ||
       (s!=ST_SHAPE_WORD&&s!=ST_SHAPE_BLOCK&&s!=ST_SHAPE_WRITE) || find(t,cmd) || t->n_regs==ST_MAX_REGS)return false;
    st_reg_t *r=&t->regs[t->n_regs++];memset(r,0,sizeof *r);r->cmd=cmd;r->shape=s;r->len=n;
    r->ready=s==ST_SHAPE_WRITE;return true;
}
bool st_set_word(st_target_t *t,uint8_t cmd,uint16_t value) {
    uint32_t g=enter(t);st_reg_t *r=find(t,cmd);bool ok=r&&r->shape==ST_SHAPE_WORD;
    if(ok){r->data[0]=(uint8_t)value;r->data[1]=(uint8_t)(value>>8);r->ready=true;t->generation++;}
    leave(t,g);return ok;
}
/* Several WORD registers replaced under ONE guard (all-or-nothing map update). A response already started keeps its own frame and PEC;
 * separate command reads (e.g. 0x0D then 0x0F) may still come from different generations - no session lock is implied. */
bool st_set_words(st_target_t *t,uint8_t n,const uint8_t *cmds,const uint16_t *vals) {
    if(!n||n>4||!cmds||!vals)return false;
    uint32_t g=enter(t);bool ok=true;st_reg_t *r[4];
    for(uint8_t k=0;k<n&&ok;k++){r[k]=find(t,cmds[k]);ok=r[k]&&r[k]->shape==ST_SHAPE_WORD;}
    if(ok){for(uint8_t k=0;k<n;k++){r[k]->data[0]=(uint8_t)vals[k];r[k]->data[1]=(uint8_t)(vals[k]>>8);r[k]->ready=true;}t->generation++;}
    leave(t,g);return ok;
}
bool st_set_block(st_target_t *t,uint8_t cmd,const uint8_t *p,uint8_t n) {
    uint32_t g=enter(t);st_reg_t *r=find(t,cmd);bool ok=p&&r&&r->shape==ST_SHAPE_BLOCK&&n==r->len;
    if(ok){memcpy(r->data,p,n);r->ready=true;t->generation++;}leave(t,g);return ok;
}
bool st_seal(st_target_t *t) {
    if(t->addr7!=0x0B || !t->n_regs)return false;
    for(unsigned k=0;k<t->n_regs;k++)if(!t->regs[k].ready)return false;
    t->sealed=true;return true;
}
bool st_live_ready(const st_target_t *t){return t->sealed&&t->enter&&t->leave;}
TE_RAMFUNC bool st_on_rx(st_target_t *t,uint8_t b) {
    if(!t->sealed || t->reading || t->invalid){t->last=ST_ABORTED;return false;}
    t->active=true;t->rx_bytes++;
    if(t->nw==0){t->last_cmd=b;t->have_last_cmd=true;}
    if(t->nw>=sizeof t->w){t->invalid=true;t->bad_len++;t->last=ST_BAD_LENGTH;return false;}
    t->w[t->nw++]=b;return true;
}
#if CHIPLER_OWNER_PHONE_TEST
/* OWNER build: a read of a command we have no evidence for (bench 2026-10-27: 0x60 x29) is answered with 0xFF on
 * every byte - exactly what the phone sees from a released bus, so no data is invented - and the transaction ends
 * normally instead of isolating the port and dropping the whole gauge for the phone. */
TE_RAMFUNC static bool refuse_read(st_target_t *t,st_result_t why){
    t->unknown_reads++;t->last=why;
    t->tx_off=0;t->wire_len=0;t->reading=true;t->nacked=false;t->overread=false;t->idle_fill=true;return true;
}
#define REFUSE_READ(t,why) return refuse_read(t,why)
#else
#define REFUSE_READ(t,why) do{(t)->unknown_reads++;(t)->last=(why);return false;}while(0)
#endif
TE_RAMFUNC bool st_begin_read(st_target_t *t) {
    if(!t->sealed || !t->active || t->reading || t->nw!=1 || t->invalid){
        t->last=ST_ABORTED;return false;
    }
    /* R2/R3 + Codex note: the evidence gate is enforced in the read path itself; a page
     * target is answered only from the selector state latched by the preceding write. */
#if CHIPLER_AUTO_LEARN
    if(t->listen){REFUSE_READ(t,ST_UNSUPPORTED);}   /* AUTO observe: no template byte before a profile is selected */
#endif
    if(t->hooks.allow_read && !t->hooks.allow_read(t->hooks_ctx,t->w[0])){
        REFUSE_READ(t,ST_UNSUPPORTED);
    }
    if(t->hooks.is_page_target && t->hooks.is_page_target(t->hooks_ctx,t->w[0])){
        const uint8_t *pd=NULL;uint8_t pc=0;
        if(!t->hooks.page_read || !t->hooks.page_read(t->hooks_ctx,t->w[0],&pd,&pc) || !pd || !pc){
            REFUSE_READ(t,ST_UNSUPPORTED);   /* no stale page */
        }
        t->wire[0]=pc;st_copy(t->wire+1,pd,pc);uint8_t np=(uint8_t)(pc+1);
        t->wire[np]=st_pec_read(t->addr7,t->w[0],t->wire,np);t->wire_len=(uint8_t)(np+1);
        t->tx_off=0;t->reading=true;t->nacked=false;t->overread=false;
        t->latched_generation=t->generation;t->last=ST_OK;return true;
    }
    st_reg_t *r=find(t,t->w[0]);
    if(!r || r->shape==ST_SHAPE_WRITE || !r->ready){REFUSE_READ(t,ST_UNSUPPORTED);}
    uint8_t n=0;if(r->shape==ST_SHAPE_BLOCK)t->wire[n++]=r->len;
    st_copy(t->wire+n,r->data,r->len);n=(uint8_t)(n+r->len);
    t->wire[n]=st_pec_read(t->addr7,r->cmd,t->wire,n);t->wire_len=(uint8_t)(n+1);
    t->tx_off=0;t->reading=true;t->nacked=false;t->overread=false;
    t->latched_generation=t->generation;t->last=ST_OK;return true;
}
TE_RAMFUNC bool st_on_tx(st_target_t *t,uint8_t *out) {
    if(!out || !t->reading || t->nacked){t->last=ST_ABORTED;return false;}
    if(t->idle_fill){*out=0xFF;t->tx_bytes++;return true;}   /* OWNER refused read: released-bus value, any length */
    if(t->tx_off>=t->wire_len){t->overread=true;t->overreads++;t->last=ST_OVERREAD;return false;}
    *out=t->wire[t->tx_off++];t->tx_bytes++;return true;
}
TE_RAMFUNC void st_on_nack(st_target_t *t){if(t->reading)t->nacked=true;}
TE_RAMFUNC static st_result_t commit_write(st_target_t *t) {
    if(t->nw<4)return ST_BAD_LENGTH;
    st_reg_t *r=find(t,t->w[0]);
    if(!r || r->shape!=ST_SHAPE_WRITE)return ST_UNSUPPORTED;
    unsigned wire_body=t->nw-2; /* excludes command and PEC, includes optional count */
    bool word=r->len==2 && wire_body==2;
    bool block=r->len!=2 && wire_body==(unsigned)r->len+1 && t->w[1]==r->len;
    if(!word&&!block)return ST_BAD_LENGTH;
    if(st_pec_write(t->addr7,t->w[0],t->w+1,wire_body)!=t->w[t->nw-1])return ST_BAD_PEC;
    /* R1/R3: protocol state (selector page) is updated here - inside the STOP path and
     * before any later START can be decoded - not from the diagnostic drain. */
    if(t->hooks.page_select)(void)t->hooks.page_select(t->hooks_ctx,t->w[0],t->w+1,wire_body);
    unsigned h=atomic_load_explicit(&t->write_head,memory_order_relaxed);
    unsigned tail=atomic_load_explicit(&t->write_tail,memory_order_acquire);
    if(h-tail>=ST_WRITE_QUEUE){t->queue_dropped++;return ST_QUEUE_FULL;}
    st_write_t *w=&t->writes[h%ST_WRITE_QUEUE];w->cmd=t->w[0];w->len=(uint8_t)wire_body;
    st_copy(w->body,t->w+1,wire_body);w->sequence=t->writes_ok+1;
    atomic_store_explicit(&t->write_head,h+1,memory_order_release);t->writes_ok++;return ST_OK;
}
bool st_get_word(const st_target_t *t,uint8_t cmd,uint16_t *v){
    if(!t||!v)return false;
    for(unsigned k=0;k<t->n_regs;k++){const st_reg_t *r=&t->regs[k];
        if(r->cmd==cmd&&r->shape==ST_SHAPE_WORD&&r->ready){*v=(uint16_t)(r->data[0]|((uint16_t)r->data[1]<<8));return true;}}
    return false;
}
TE_RAMFUNC bool st_resume_pointer(st_target_t *t) {
    if(!t->sealed||t->active||t->reading||!t->ptr_valid){t->last=ST_ABORTED;return false;}
    t->active=true;t->w[0]=t->ptr;t->nw=1;return true;   /* then st_begin_read() re-validates the command */
}
TE_RAMFUNC st_result_t st_on_stop(st_target_t *t) {
    st_result_t r=ST_OK;
    if(!t->active){reset_txn(t);return t->last=ST_ABORTED;}
    t->txns++;
    if(t->invalid)r=ST_BAD_LENGTH;
    else if(t->reading){
        if(t->idle_fill)r=ST_OK;   /* OWNER refused read already counted in unknown_reads; end it normally */
        else if(t->overread)r=ST_OVERREAD;
        else if(!t->nacked)r=ST_ABORTED;
        else if(t->tx_off==t->wire_len){t->reads_done++;r=ST_OK;}
        else if(t->w[0]==0xA8&&t->wire_len==26&&t->tx_off==1&&t->wire[0]==24){t->reads_short++;r=ST_SHORT_A8;}
#if CHIPLER_OWNER_PHONE_TEST
        /* OWNER build: the iPhone 12 boot capture reads the byte count of a block first, NACKs, then re-reads the
         * whole block (0x7E/0x21/0x90/0x91/0x95/0x76/0x9E/0xA8/0x74, first_burst_4.13s.txt). Accept a count-only read
         * of any BLOCK register (wire = count + data + PEC) as the same short probe as 0xA8. */
        else if(t->tx_off==1&&t->wire_len>=3&&t->wire[0]==(uint8_t)(t->wire_len-2)){t->reads_short++;r=ST_SHORT_A8;}
#endif
        else{t->reads_short++;r=ST_BAD_LENGTH;}
    }else if(t->nw==1){r=ST_OK;} /* pointer-only STOP: remembered below only if the command is known */
    else r=commit_write(t);
    bool data_write=!t->reading&&t->nw>1;
    {   /* R3 pointer policy */
        bool done_read=t->reading&&(r==ST_OK||r==ST_SHORT_A8);
        bool ptr_only=!t->reading&&t->nw==1&&r==ST_OK;
        st_reg_t *pr=(done_read||ptr_only)?find(t,t->w[0]):NULL;
        if(pr&&pr->shape!=ST_SHAPE_WRITE&&pr->ready){t->ptr=t->w[0];t->ptr_valid=true;}
        else t->ptr_valid=false;
    }
#if CHIPLER_AUTO_LEARN
    if(t->nw>=1){   /* one observation per transaction that named a command: what the phone asked, and what we did */
        uint32_t kind=t->reading?1u:(t->nw==1?0u:2u);
        bool refused=t->reading&&t->idle_fill, served=t->reading&&!t->idle_fill&&(r==ST_OK||r==ST_SHORT_A8);
        uint32_t len=t->reading?t->tx_off:t->nw;
        uint32_t o=(uint32_t)t->w[0]|(kind<<8)|((served?1u:0u)<<10)|((refused?1u:0u)<<11)|((len>255u?255u:len)<<16);
        unsigned h=atomic_load_explicit(&t->obs_head,memory_order_relaxed);
        if(h-atomic_load_explicit(&t->obs_tail,memory_order_acquire)>=ST_OBS_RING)t->obs_dropped++;
        else{t->obs[h%ST_OBS_RING]=o;atomic_store_explicit(&t->obs_head,h+1,memory_order_release);}
    }
#endif
    if(r==ST_BAD_LENGTH&&!t->invalid)t->bad_len++;
    if(r==ST_ABORTED)t->aborts++;
    if(r!=ST_OK&&r!=ST_SHORT_A8&&!t->reading)t->writes_bad++;
    t->last=r;reset_txn(t);
#if CHIPLER_OWNER_PHONE_TEST
    /* OWNER build: a write we cannot use (unknown command e.g. 0x7A, bad length/PEC, or the log-only write queue
     * full under 0x65 page-select bursts) is accounted exactly like the default build above (writes_bad, bad_len,
     * queue_dropped, last) and then dropped instead of isolating the port. Writes never change the served data except
     * through the selector hook, which already ran inside commit_write. (review 102 F-03: the earlier version converted
     * the result before the accounting and lost bad_len/last.) */
    if(data_write&&r!=ST_OK)r=ST_OK;
#else
    (void)data_write;
#endif
    return r;
}
#if CHIPLER_AUTO_LEARN
void st_set_listen(st_target_t *t,bool on){uint32_t g=enter(t);t->listen=on;leave(t,g);}
bool st_pop_obs(st_target_t *t,uint32_t *out){
    unsigned q=atomic_load_explicit(&t->obs_tail,memory_order_relaxed);
    unsigned h=atomic_load_explicit(&t->obs_head,memory_order_acquire);if(q==h||!out)return false;
    *out=t->obs[q%ST_OBS_RING];atomic_store_explicit(&t->obs_tail,q+1,memory_order_release);return true;
}
#endif
TE_RAMFUNC void st_on_restart_write(st_target_t *t) {if(t->active){t->aborts++;t->last=ST_ABORTED;}reset_txn(t);}
TE_RAMFUNC void st_abort(st_target_t *t) {uint32_t g=enter(t);if(t->active)t->aborts++;reset_txn(t);t->ptr_valid=false;t->last=ST_ABORTED;leave(t,g);}
bool st_pop_write(st_target_t *t,st_write_t *out) {
    if(!out)return false;
    unsigned q=atomic_load_explicit(&t->write_tail,memory_order_relaxed);
    unsigned h=atomic_load_explicit(&t->write_head,memory_order_acquire);if(q==h)return false;
    *out=t->writes[q%ST_WRITE_QUEUE];atomic_store_explicit(&t->write_tail,q+1,memory_order_release);return true;
}
