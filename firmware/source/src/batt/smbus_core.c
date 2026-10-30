#include "smbus_core.h"
#include <string.h>
_Static_assert(ATOMIC_CHAR_LOCK_FREE==2,"snapshot index must be lock-free in IRQ context");
_Static_assert(ATOMIC_BOOL_LOCK_FREE==2,"ready flag must be lock-free in IRQ context");
static uint8_t crc_step(uint8_t c, uint8_t b) {
    c ^= b;
    for (int k=0;k<8;k++) c=(uint8_t)((c<<1)^((c&128)?7:0));
    return c;
}
uint8_t smb_crc8(const uint8_t *p,size_t n) {
    uint8_t c=0; for(size_t k=0;k<n;k++) c=crc_step(c,p[k]); return c;
}
smb_result_t smb_validate(uint8_t addr,const uint8_t *w,size_t nw,const uint8_t *r,size_t nr,bool read,bool framing) {
    if (!framing) return SMB_ABORTED;
    if (addr==0x71) return SMB_OFFLINE_71; /* bytes retained by fixture; no checksum claim */
    if (addr!=0x0B) return SMB_UNSUPPORTED;
    if (!w || nw==0 || nw>35 || (nr && !r)) return SMB_BAD_LENGTH;
    uint8_t c=crc_step(0,(uint8_t)(addr<<1));
    if (read) {
        if(nw!=1 || nr==0 || nr>34) return SMB_BAD_LENGTH;
        if(w[0]==0xA8 && nr==1 && r[0]==0x18) return SMB_SHORT_A8;
        /* Captures distinguish a word (2 data + PEC) from a counted block. */
        if(nr!=3 && !(nr>=3 && r[0]>=1 && r[0]<=32 && nr==(size_t)r[0]+2)) return SMB_BAD_LENGTH;
        c=crc_step(c,w[0]); c=crc_step(c,(uint8_t)((addr<<1)|1));
        for(size_t k=0;k+1<nr;k++) c=crc_step(c,r[k]);
        return c==r[nr-1] ? SMB_OK : SMB_BAD_PEC;
    }
    if(nw!=4 && !(nw>=4 && w[1]>=1 && w[1]<=32 && nw==(size_t)w[1]+3)) return SMB_BAD_LENGTH;
    for(size_t k=0;k+1<nw;k++) c=crc_step(c,w[k]);
    return c==w[nw-1] ? SMB_OK : SMB_BAD_PEC;
}
void smb_init(smbus_t *s) { memset(s,0,sizeof *s); atomic_init(&s->published,0);atomic_init(&s->prepared,false); }
bool smb_publish(smbus_t *s,uint8_t addr,uint8_t cmd,smb_kind_t kind,const uint8_t *d,size_t n) {
    if(addr!=0x0B || !d || (kind!=SMB_WORD && kind!=SMB_BLOCK) ||
       (kind==SMB_WORD ? n!=2 : n<1 || n>32)) return false;
    uint8_t slot=(uint8_t)(atomic_load_explicit(&s->published,memory_order_relaxed)^1);
    smb_image_t *i=&s->image[slot];
    i->addr=addr;i->cmd=cmd;i->kind=kind;i->count=(uint8_t)n;
    memcpy(i->data,d,n);i->generation=++s->generation;
    atomic_store_explicit(&s->published,slot,memory_order_release);
    atomic_store_explicit(&s->prepared,true,memory_order_release); return true;
}
bool smb_begin_read(smbus_t *s,uint8_t addr,uint8_t cmd) {
    if(s->reading || !atomic_load_explicit(&s->prepared,memory_order_acquire)) {s->unsupported++;return false;}
    smb_image_t i=s->image[atomic_load_explicit(&s->published,memory_order_acquire)];
    if(addr!=0x0B || addr!=i.addr || cmd!=i.cmd) {s->unsupported++;return false;}
    size_t n=0;
    if(i.kind==SMB_BLOCK) s->wire[n++]=i.count;
    memcpy(s->wire+n,i.data,i.count);n+=i.count;
    uint8_t c=crc_step(crc_step(crc_step(0,(uint8_t)(addr<<1)),cmd),(uint8_t)((addr<<1)|1));
    for(size_t k=0;k<n;k++) c=crc_step(c,s->wire[k]);
    s->wire[n++]=c;s->wire_len=(uint8_t)n;s->offset=0;s->reading=true;s->overread=false;s->latched_cmd=cmd;
    s->latched_generation=i.generation;return true;
}
bool smb_read_byte(smbus_t *s,uint8_t *b) {
    if(!b || !s->reading) return false;
    if(s->offset>=s->wire_len) {s->overread=true;return false;}
    *b=s->wire[s->offset++];return true;
}
smb_result_t smb_end_read(smbus_t *s,bool nack,bool abort) {
    bool short_a8=s->reading && s->latched_cmd==0xA8 && s->wire_len==26 && s->offset==1 && s->wire[0]==0x18 && !s->overread;
    bool complete=s->reading && s->offset==s->wire_len && !s->overread;
    s->reading=false;
    if(abort || !nack) return SMB_ABORTED;
    if(complete) return SMB_OK;
    return short_a8 ? SMB_SHORT_A8 : SMB_BAD_LENGTH;
}
smb_result_t smb_commit_write(smbus_t *s,uint8_t addr,const uint8_t *w,size_t n,bool stop) {
    smb_result_t result=smb_validate(addr,w,n,0,0,false,stop);
    if(result!=SMB_OK) {s->rejects++;return result;}
    /* Record opaque payload only after complete validated STOP; no selector/auth semantics. */
    size_t len=n-2, offset=1;
    s->last_write_cmd=w[0];s->last_write_len=(uint8_t)len;
    memcpy(s->last_write,w+offset,len);s->commits++;return SMB_OK;
}
void smb_abort(smbus_t *s) {s->reading=false;s->offset=0;s->wire_len=0;}
void smb_stream_init(smb_stream_t *s,smbus_t *target) {memset(s,0,sizeof *s);s->target=target;}
void smb_stream_start(smb_stream_t *s,uint8_t addr,bool read,bool repeated,bool ack,uint32_t now) {
    if(!repeated) {
        smbus_t *target=s->target;
        if(target)smb_abort(target);
        smb_stream_init(s,target);s->framing=true;
    }
    else if(!s->active || s->read || !read || s->addr!=addr || s->nw!=1) s->framing=false;
    s->active=true;s->addr=addr;s->read=read;s->last_ms=now;s->framing &= ack;
}
void smb_stream_byte(smb_stream_t *s,uint8_t b,bool ack,uint32_t now) {
    if(!s->active)return;
    s->last_ms=now;
    if(s->nack_seen)s->framing=false;
    if(s->read) {
        if(s->nr<sizeof s->r)s->r[s->nr++]=b;else s->framing=false;
        if(!ack)s->nack_seen=true;
    } else {
        if(s->nw<sizeof s->w)s->w[s->nw++]=b;else s->framing=false;
        if(!ack)s->framing=false;
    }
}
smb_result_t smb_stream_stop(smb_stream_t *s,uint32_t now) {
    bool framing=s->active&&s->framing&&(!s->read||s->nack_seen);s->last_ms=now;
    smb_result_t result=smb_validate(s->addr,s->w,s->nw,s->r,s->nr,s->read,framing);
    if(result==SMB_OK&&!s->read&&s->target) result=smb_commit_write(s->target,s->addr,s->w,s->nw,true);
    s->active=false;return result;
}
bool smb_stream_expire(smb_stream_t *s,uint32_t now,uint32_t max_idle) {
    if(!s->active||(uint32_t)(now-s->last_ms)<=max_idle)return false;
    s->active=false;s->nw=s->nr=0;s->framing=false;s->nack_seen=false;
    if(s->target)smb_abort(s->target);
    return true;
}
