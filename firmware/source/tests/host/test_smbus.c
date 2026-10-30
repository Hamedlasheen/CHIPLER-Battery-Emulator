#include "../../src/batt/smbus_core.h"
#include "../../src/batt/capture_replay.h"
#include "../../src/batt/target_prepare.h"
#include <string.h>
static void test_smbus(void) {
    const uint8_t vector[]={0x16,9,0x17,0x46,0x0F};CHECK(smb_crc8(vector,5)==0x63);
    uint8_t cmd=9,word[]={0x46,0x0F,0x63};
    CHECK(smb_validate(0x0B,&cmd,1,word,3,true,true)==SMB_OK);
    CHECK(smb_validate(0x71,&cmd,1,word,3,true,true)==SMB_OFFLINE_71);
    CHECK(smb_validate(0x0C,&cmd,1,word,3,true,true)==SMB_UNSUPPORTED);
    for(unsigned bit=0;bit<24;bit++) {
        uint8_t bad[3];memcpy(bad,word,3);bad[bit/8]^=(uint8_t)(1u<<(bit%8));
        CHECK(smb_validate(0x0B,&cmd,1,bad,3,true,true)==SMB_BAD_PEC);
    }
    CHECK(smb_validate(0x0B,&cmd,1,word,2,true,true)==SMB_BAD_LENGTH);
    CHECK(smb_validate(0x0B,&cmd,1,word,3,true,false)==SMB_ABORTED);
    smbus_t e;smb_init(&e);uint8_t data[32];for(int k=0;k<32;k++)data[k]=(uint8_t)k;
    char logmem[160];log_ring_t blocked_log;lr_init(&blocked_log,logmem,sizeof logmem);
    CHECK(!smb_begin_read(&e,0x0B,9));CHECK(!smb_publish(&e,0x71,9,SMB_WORD,data,2));
    CHECK(!smb_publish(&e,0x0B,9,SMB_BLOCK,data,0));CHECK(!smb_publish(&e,0x0B,9,SMB_BLOCK,data,33));
    const unsigned lens[]={4,24,32};
    for(unsigned z=0;z<3;z++) {
        unsigned n=lens[z];CHECK(smb_publish(&e,0x0B,0xA8,SMB_BLOCK,data,n));
        for(unsigned stop=0;stop<=n+2;stop++) {
            CHECK(smb_begin_read(&e,0x0B,0xA8));uint8_t wire[34]={0};
            uint32_t latched=e.latched_generation;
            for(unsigned k=0;k<stop;k++) {
                CHECK(smb_read_byte(&e,&wire[k]));
                /* Publication between EVERY byte cannot alter latched data or PEC. */
                uint8_t other[32];memset(other,0xAA,n);CHECK(smb_publish(&e,0x0B,0xA8,SMB_BLOCK,other,n));
                CHECK(e.latched_generation==latched);
                lr_printf(&blocked_log,k,"SMB","generation=%lu",(unsigned long)e.generation);
                if(k==0)CHECK(wire[k]==n);
                else if(k<=n)CHECK(wire[k]==data[k-1]);
            }
            smb_result_t wanted=stop==n+2?SMB_OK:(stop==1&&n==24?SMB_SHORT_A8:SMB_BAD_LENGTH);
            CHECK(smb_end_read(&e,true,false)==wanted);
            if(stop==n+2)CHECK(smb_validate(0x0B,(uint8_t[]){0xA8},1,wire,stop,true,true)==SMB_OK);
            CHECK(smb_publish(&e,0x0B,0xA8,SMB_BLOCK,data,n));
        }
        CHECK(smb_begin_read(&e,0x0B,0xA8));uint8_t b;
        for(unsigned k=0;k<n+2;k++)CHECK(smb_read_byte(&e,&b));
        CHECK(!smb_read_byte(&e,&b));CHECK(smb_end_read(&e,true,false)==SMB_BAD_LENGTH);
    }
    CHECK(blocked_log.dropped>0&&lr_used(&blocked_log)<=sizeof logmem);
    CHECK(smb_publish(&e,0x0B,0x66,SMB_BLOCK,data,24));CHECK(smb_begin_read(&e,0x0B,0x66));
    uint8_t b;CHECK(smb_read_byte(&e,&b));CHECK(smb_end_read(&e,true,false)==SMB_BAD_LENGTH); /* byte18 alone is not A8 */
    CHECK(smb_begin_read(&e,0x0B,0x66));smb_abort(&e);CHECK(!smb_read_byte(&e,&b));
    uint8_t write[]={0x65,0,0,0};write[3]=smb_crc8((uint8_t[]){0x16,0x65,0,0},4);
    CHECK(smb_commit_write(&e,0x0B,write,4,true)==SMB_OK);uint32_t commits=e.commits;
    write[3]^=1;CHECK(smb_commit_write(&e,0x0B,write,4,true)==SMB_BAD_PEC&&e.commits==commits);
    CHECK(smb_commit_write(&e,0x0B,write,3,true)==SMB_BAD_LENGTH&&e.commits==commits);
    write[3]^=1;CHECK(smb_commit_write(&e,0x0B,write,4,false)==SMB_ABORTED&&e.commits==commits);
    /* Count violations with a correctly recomputed PEC, not only checksum corruption. */
    for(unsigned count=0;count<=33;count+=33) {
        uint8_t bad[35]={0},covered[37]={0x16,9,0x17};bad[0]=(uint8_t)count;
        unsigned nr=count+2;memcpy(covered+3,bad,nr-1);bad[nr-1]=smb_crc8(covered,nr+2);
        CHECK(smb_validate(0x0B,&cmd,1,bad,nr,true,true)==SMB_BAD_LENGTH);
    }
    capture_replay_start();bool done=false;unsigned steps=0;
    while(!done&&steps++<capture_replay_count()+1) done=capture_replay_step(1);
    capture_stats_t s=capture_replay_stats();
    CHECK(done&&s.frames==capture_replay_count()&&s.errors==0);
    CHECK(s.valid==3530&&s.offline71==4104&&s.short_a8==19&&s.rejected==670);
    CHECK(!capture_replay_step(16));
    CHECK(smb_publish(&e,0x0B,0xA8,SMB_WORD,(uint8_t[]){0x18,0},2));
    CHECK(smb_begin_read(&e,0x0B,0xA8)&&smb_read_byte(&e,&b));
    CHECK(smb_end_read(&e,true,false)==SMB_BAD_LENGTH); /* only a counted24-byte A8 has a short probe */
    CHECK(smb_publish(&e,0x0B,0x66,SMB_BLOCK,data,24));
    smb_stream_t stream;smb_stream_init(&stream,&e);commits=e.commits;
    CHECK(smb_begin_read(&e,0x0B,0x66));
    smb_stream_start(&stream,0x0B,false,false,true,UINT32_MAX-10);
    CHECK(!e.reading); /* new START discards any aborted previous response */
    smb_stream_byte(&stream,0x65,true,UINT32_MAX-10);
    CHECK(!smb_stream_expire(&stream,9,20));
    CHECK(smb_stream_expire(&stream,10,20)&&e.commits==commits);
    CHECK(smb_stream_stop(&stream,11)==SMB_ABORTED&&e.commits==commits);
    smb_stream_start(&stream,0x0B,false,false,true,100);
    smb_stream_byte(&stream,9,true,101);
    smb_stream_start(&stream,0x0B,true,true,true,102);
    for(unsigned k=0;k<3;k++)smb_stream_byte(&stream,word[k],k<2,103+k);
    CHECK(smb_stream_stop(&stream,106)==SMB_OK);
    smb_stream_start(&stream,0x0B,false,false,true,200);
    for(unsigned k=0;k<4;k++)smb_stream_byte(&stream,write[k],true,201+k);
    CHECK(e.commits==commits); /* STOP is the commit boundary */
    CHECK(smb_stream_stop(&stream,205)==SMB_OK&&e.commits==commits+1);
    smb_stream_start(&stream,0x0B,false,false,true,300);
    for(unsigned k=0;k<40;k++)smb_stream_byte(&stream,0,true,301+k);
    CHECK(smb_stream_stop(&stream,350)==SMB_ABORTED&&e.commits==commits+1);
}
static bool fail_builder(int soc,uint8_t *d,uint8_t *base,uint8_t *n) {(void)soc;(void)d;(void)base;*n=0;return false;}
static void test_target_prepare(void) {
    target_engine_t e;const batt_profile_t *p=profile_get(profile_find("BENCH-r0"));
    /* Legacy preparation is tested against the legacy profile, not the new SMBus modes. */
    CHECK(p&&target_prepare(&e,p,50)==TP_OK);
    te_on_receive(&e,0x20,0);CHECK(te_on_request(&e,0)==50);te_on_finish(&e);
    CHECK(target_prepare(&e,NULL,50)==TP_BAD_PROFILE&&e.prof==NULL);
    batt_profile_t bad=*p;bad.i2c_addr=0;CHECK(target_prepare(&e,&bad,50)==TP_BAD_ADDRESS&&e.prof==NULL);
    bad=*p;bad.soc_build=fail_builder;CHECK(target_prepare(&e,&bad,50)==TP_BUILD_FAILED&&e.prof==NULL);
    bad=*p;bad.soc_build=NULL;bad.n_regs=0;CHECK(target_prepare(&e,&bad,50)==TP_NO_RESPONSE);
    CHECK(target_prepare(&e,profile_get(profile_find("P12PM-r0")),50)==TP_BAD_PROFILE);
    CHECK(profile_count_armable()==expect_armable_count());
}
