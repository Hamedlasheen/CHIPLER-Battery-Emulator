#include "capture_replay.h"
#include "smbus_core.h"
#include <string.h>
typedef struct { uint8_t addr; uint16_t nw,nr; uint8_t flags,expected,w[35],r[35]; } capture_frame_t;
#include "capture_data.inc"
static capture_stats_t stats;
static unsigned cursor;
static bool running;
static smbus_t engine;
void capture_replay_start(void) {memset(&stats,0,sizeof stats);cursor=0;running=true;smb_init(&engine);}
unsigned capture_replay_count(void) {return (unsigned)CAPTURE_FRAME_COUNT;}
capture_stats_t capture_replay_stats(void) {return stats;}
bool capture_replay_step(unsigned budget) {
    if(!running)return false;
    if(budget>16)budget=16;
    while(budget-- && cursor<CAPTURE_FRAME_COUNT) {
        const capture_frame_t *f=&capture_frames[cursor++];
        smb_result_t result=smb_validate(f->addr,f->w,f->nw,f->r,f->nr,(f->flags&1)!=0,(f->flags&2)!=0);
        stats.frames++;
        if(result!=f->expected)stats.errors++;
        if(result==SMB_OK) {
            stats.valid++;
            smb_stream_t stream;smb_stream_init(&stream,&engine);
            smb_stream_start(&stream,f->addr,false,false,true,0);
            for(unsigned k=0;k<f->nw;k++)smb_stream_byte(&stream,f->w[k],true,k);
            if(f->flags&1) {
                smb_stream_start(&stream,f->addr,true,true,true,1);
                for(unsigned k=0;k<f->nr;k++)smb_stream_byte(&stream,f->r[k],k+1<f->nr,k+1);
            }
            if(smb_stream_stop(&stream,100)!=SMB_OK)stats.errors++;
            if(f->flags&1) {
                smb_kind_t kind=f->nr==3?SMB_WORD:SMB_BLOCK;
                const uint8_t *payload=f->r+(kind==SMB_BLOCK?1:0);
                unsigned n=kind==SMB_BLOCK?f->r[0]:2;
                if(!smb_publish(&engine,f->addr,f->w[0],kind,payload,n)||!smb_begin_read(&engine,f->addr,f->w[0]))stats.errors++;
                else {
                    for(unsigned k=0;k<f->nr;k++) {uint8_t b=0;if(!smb_read_byte(&engine,&b)||b!=f->r[k])stats.errors++;}
                    if(smb_end_read(&engine,true,false)!=SMB_OK)stats.errors++;
                }
            }
        } else if(result==SMB_OFFLINE_71)stats.offline71++;
        else if(result==SMB_SHORT_A8)stats.short_a8++;
        else stats.rejected++;
    }
    if(cursor==CAPTURE_FRAME_COUNT) {running=false;return true;}return false;
}
