/* Compiles the production HAL against a fake Pico peripheral. No hardware PASS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/hal/battery_bus.h"
#include "board_config.h"
#include "pico/i2c_slave.h"
#include "../../src/hal/live_port.h"
#include "../../src/batt/live_wire.h"
#include "../../src/batt/owner_words.h"
#include "owner_policy.h"
static unsigned checks,fails,enabled_calls;static bool isolated=true;static uint8_t wanted,rx,tx;static bool have_rx;
static bool live_fault;static st_target_t *live_target;
/* review 105 R1/R2: what the first phone request inside live_port_start() sees. mode 0 = bench block 0x20 (as before);
 * mode 1 = the phone's first boot reads 0x0D, 0x0F, 0x09 (WORD + PEC), answers kept in first[] for the checks. */
static int start_mode;static bool start_refuse;static uint16_t first[3];static bool first_pec_ok[3];static unsigned started;
static uint8_t crc8(const uint8_t *p,unsigned n){uint8_t c=0;for(unsigned i=0;i<n;i++){c^=p[i];for(int b=0;b<8;b++)c=(uint8_t)((c&0x80)?(c<<1)^0x07:(c<<1));}return c;}
static bool phone_word(st_target_t *t,uint8_t cmd,uint16_t *v,bool *pec_ok){   /* full SMBus ReadWord with PEC */
 uint8_t d[2],pec,pre[5]={0x16,cmd,0x17};
 if(!st_on_rx(t,cmd)||!st_begin_read(t)||!st_on_tx(t,&d[0])||!st_on_tx(t,&d[1])||!st_on_tx(t,&pec))return false;
 st_on_nack(t);if(st_on_stop(t)!=ST_OK)return false;
 pre[3]=d[0];pre[4]=d[1];*v=(uint16_t)(d[0]|d[1]<<8);*pec_ok=pec==crc8(pre,5);return true;
}
#define CHECK(x) do{checks++;if(!(x)){fails++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
i2c_inst_t fake_i2c0={0},fake_i2c1={1};
uint32_t time_us_32(void){return 0;}
void gpio_init(unsigned p){(void)p;}void gpio_set_dir(unsigned p,bool d){(void)p;(void)d;}
void gpio_put(unsigned p,bool on){if(p==PIN_BAT_BUS_EN){if(on)CHECK(battery_bus_engine()->prof!=NULL);isolated=!on;}}
void gpio_disable_pulls(unsigned p){(void)p;}void gpio_set_function(unsigned p,unsigned f){(void)p;(void)f;}
void i2c_init(i2c_inst_t *i,unsigned hz){(void)i;(void)hz;CHECK(isolated);}
void i2c_deinit(i2c_inst_t *i){(void)i;}
unsigned i2c_get_read_available(i2c_inst_t *i){(void)i;return have_rx?1:0;}
uint8_t i2c_read_byte_raw(i2c_inst_t *i){(void)i;have_rx=false;return rx;}
void i2c_write_byte_raw(i2c_inst_t *i,uint8_t v){(void)i;tx=v;}
unsigned i2c_hw_index(i2c_inst_t *i){return i->index;}
void irq_set_priority(unsigned i,unsigned p){(void)i;(void)p;}
uint32_t live_port_guard_enter(void *p){(void)p;return 0;}
void live_port_guard_exit(void *p,uint32_t x){(void)p;(void)x;}
bool live_port_fault(void){return live_fault;}
void live_port_clear_fault(void){live_fault=false;}
void live_port_diagnostic(st_result_t *r,uint32_t *irq,uint32_t *src){*r=ST_OK;*irq=*src=0;}
void live_port_stop(void){gpio_put(PIN_BAT_BUS_EN,0);if(live_target)st_abort(live_target);live_target=0;}
void live_port_service(uint32_t now){(void)now;}
void live_port_history(uint32_t out[8],uint8_t *site){for(int i=0;i<8;i++)out[i]=0;*site=0;}
uint32_t live_port_last_event_us(void){return 0;}
bool live_port_start(st_target_t *t){
 CHECK(isolated&&st_live_ready(t));
 if(start_refuse)return false;
 live_target=t;enabled_calls++;started++;
 if(start_mode==1){   /* the phone reads the instant the target is enabled, before activate() returns */
  static const uint8_t cmds[3]={0x0D,0x0F,0x09};
  for(int k=0;k<3;k++)CHECK(phone_word(t,cmds[k],&first[k],&first_pec_ok[k]));
  gpio_put(PIN_BAT_BUS_EN,1);return true;
 }
 /* First SMBus request before activation returns; response already complete. */
 CHECK(st_on_rx(t,0x20)&&st_begin_read(t));uint8_t b=0;
 CHECK(st_on_tx(t,&b)&&b==4);CHECK(st_on_tx(t,&b)&&b==wanted);
 while(st_on_tx(t,&b)){} /* deliberate overread is not success */
 st_on_nack(t);CHECK(st_on_stop(t)==ST_OVERREAD);st_abort(t);
 gpio_put(PIN_BAT_BUS_EN,1);return true;
}
/* owner_initial_words() over the production facade (what main.c's bus_initial_words passes for a DATA TEST profile) */
static bool fw_word(void *c,uint8_t cmd,uint16_t *v){(void)c;return battery_bus_word(cmd,v);}
static bool fw_pub(void *c,uint8_t b,const uint8_t *d,uint8_t n){(void)c;return battery_bus_publish(b,d,n);}
static const owner_bus_t fw_bus={fw_word,fw_pub,0};
typedef struct{int soc;bool v_ok;float v;unsigned calls;}init_t;
static bool init_words(void *ctx){init_t *i=ctx;i->calls++;CHECK(isolated&&!battery_bus_running()&&!live_target);
 return owner_initial_words(&fw_bus,i->soc,i->v_ok,i->v);}
/* R2: what can happen to the bus while a save runs inside battery_bus_pause() */
typedef struct{unsigned runs;bool target_off;}pause_t;
static void pause_job(void *ctx){pause_t *j=ctx;j->runs++;
 /* the flash write would run here: no target installed (nothing to ACK, no IRQ to service), output isolated */
 j->target_off=live_target==NULL&&isolated&&!battery_bus_running();}
static void r105_tests(void){
 const batt_profile_t *p=profile_get(profile_find("P12PM-B2"));
 CHECK(p&&p->status==PROF_PHONE_DATA_TEST);if(!p)return;
 start_mode=1;
 uint16_t fcc=0,rec_soc=0,rec_v=0;
 CHECK(battery_bus_prepare(p,50)&&battery_bus_word(0x10,&fcc)&&battery_bus_word(0x0D,&rec_soc)&&battery_bus_word(0x09,&rec_v));
 battery_bus_stop();
 CHECK(fcc&&rec_soc!=37);   /* the recorded map does not already hold the value under test */
 /* mutation / reproduction of Codex 105 R1: without the initial words the first answer is the recorded SOC */
 CHECK(battery_bus_start_seq(p,37,NULL,NULL)==BB_START_OK);
 CHECK(first[0]==rec_soc&&first[0]!=37);
 battery_bus_stop();
 /* R1 fix: restore of 37 % at boot: the very first 0x0D/0x0F/0x09 answers carry the owner values, PEC valid */
 init_t in={37,true,3.912f,0};
 CHECK(battery_bus_start_seq(p,37,init_words,&in)==BB_START_OK&&in.calls==1&&battery_bus_running());
 CHECK(first[0]==37&&first[1]==owner_remaining(fcc,37)&&first[2]==3912);
 CHECK(first_pec_ok[0]&&first_pec_ok[1]&&first_pec_ok[2]);
 /* recovery restart (owner auto-recover: reset_fault, then the policy starts again): same guarantee */
 live_fault=true;battery_bus_reset_fault();CHECK(!battery_bus_running()&&!battery_bus_fault());
 in.soc=64;in.v_ok=false;memset(first,0,sizeof first);
 CHECK(battery_bus_start_seq(p,64,init_words,&in)==BB_START_OK);
 CHECK(first[0]==64&&first[1]==owner_remaining(fcc,64)&&first[2]==rec_v);   /* no sample: recorded 0x09 kept */
 /* voltage out of 2.80..4.60 V: recorded 0x09 kept, SOC still published, start allowed */
 battery_bus_stop();in.v_ok=true;in.v=5.2f;
 CHECK(battery_bus_start_seq(p,64,init_words,&in)==BB_START_OK&&first[0]==64&&first[2]==rec_v);
 battery_bus_stop();
 /* failures keep the target off: SOC outside 1..100 (nothing usable to publish), prepare refused */
 unsigned before=started;in.soc=0;
 CHECK(battery_bus_start_seq(p,0,init_words,&in)==BB_START_WORDS&&!battery_bus_running()&&isolated&&started==before);
 in.soc=101;
 CHECK(battery_bus_start_seq(p,101,init_words,&in)==BB_START_WORDS&&!battery_bus_running()&&started==before);
 live_fault=true;in.soc=37;
 CHECK(battery_bus_start_seq(p,37,init_words,&in)==BB_START_PREPARE&&!battery_bus_running()&&started==before);
 battery_bus_reset_fault();
 CHECK(battery_bus_start_seq(p,37,init_words,&in)==BB_START_OK);
 /* ---- R2: battery_bus_pause ---- */
 pause_t j={0,false};
 /* a phone read is half done when the pause starts: the stop cuts it (the phone sees a NACK/abort, never a stretched
  * clock); after the re-arm the map is intact and the next full read is correct */
 uint8_t b;CHECK(st_on_rx(live_target,0x0D)&&st_begin_read(live_target)&&st_on_tx(live_target,&b)&&b==37);
 before=started;
 CHECK(battery_bus_pause(pause_job,&j)&&j.runs==1&&j.target_off&&battery_bus_running()&&started==before+1);
 CHECK(first[0]==37&&first[1]==owner_remaining(fcc,37)&&first_pec_ok[0]&&first_pec_ok[1]);   /* first reads after re-arm */
 { uint16_t v;bool ok;CHECK(phone_word(live_target,0x0D,&v,&ok)&&v==37&&ok&&!battery_bus_fault()); }
 /* re-arm refused: the pause reports it, the target stays off, and the normal start path (initial words) recovers */
 start_refuse=true;j.runs=0;
 CHECK(!battery_bus_pause(pause_job,&j)&&j.runs==1&&j.target_off&&!battery_bus_running());
 start_refuse=false;
 CHECK(battery_bus_start_seq(p,37,init_words,&in)==BB_START_OK&&first[0]==37);
 /* nothing to pause: stopped target, faulted target, non-live (BENCH-r0) target -> fn never runs */
 battery_bus_stop();j.runs=0;
 CHECK(!battery_bus_pause(pause_job,&j)&&j.runs==0);
 CHECK(battery_bus_start_seq(p,37,init_words,&in)==BB_START_OK);
 live_fault=true;CHECK(!battery_bus_pause(pause_job,&j)&&j.runs==0);battery_bus_reset_fault();
 start_mode=0;
 const batt_profile_t *bench=profile_get(profile_find("BENCH-r0"));wanted=50;
 CHECK(battery_bus_prepare(bench,50)&&battery_bus_activate());
 CHECK(!battery_bus_pause(pause_job,&j)&&j.runs==0&&battery_bus_running());
 battery_bus_stop();
 /* review 106: every model on the shared 12 Pro engine (registry backend field; the independent list is in
  * shared12_tests) gets its owner SOC from the very first read after activation; STOP leaves nothing running */
 unsigned n106=0;start_mode=1;
 for(int i=0;i<profile_count();i++){
  const batt_profile_t *q=profile_get(i);
  if(q->backend!=BK_SHARED_12PRO)continue;
  init_t iq={1+(i*7)%100,true,3.800f,0};memset(first,0,sizeof first);
  CHECK(battery_bus_start_seq(q,iq.soc,init_words,&iq)==BB_START_OK&&iq.calls==1);
  uint16_t f10=0;CHECK(battery_bus_word(0x10,&f10));
  CHECK(first[0]==iq.soc&&first[1]==owner_remaining(f10,(unsigned)iq.soc)&&first[2]==3800&&first_pec_ok[0]&&first_pec_ok[1]&&first_pec_ok[2]);
  battery_bus_stop();CHECK(!battery_bus_running()&&isolated);
  CHECK(!profile_bus_wanted(profile_get(profile_replies_off_index()),false));   /* STOP target: the next tick starts nothing */
  n106++;
 }
 CHECK(n106==(OWNER?19u:0u));
 start_mode=0;
 printf("review 106: %u shared-12Pro models: first read after activation = owner SOC/voltage with valid PEC\n",n106);
 printf("review 105: R1 first answers after start/restore/recovery = owner SOC with valid PEC; R2 pause runs the job with the target off\n");
}
void i2c_slave_deinit(i2c_inst_t *i){(void)i;CHECK(isolated);}
void i2c_slave_init(i2c_inst_t *i,uint8_t addr,void(*cb)(i2c_inst_t *,i2c_slave_event_t)){
    enabled_calls++;CHECK(isolated);CHECK(addr==battery_bus_engine()->prof->i2c_addr);
    /* Inject the first request INSIDE enable, before activation returns or GP5 rises. */
    rx=0x20;have_rx=true;cb(i,I2C_SLAVE_RECEIVE);cb(i,I2C_SLAVE_REQUEST);CHECK(tx==wanted);cb(i,I2C_SLAVE_FINISH);
}
int main(void){
    const batt_profile_t *p=NULL;
    p=profile_get(profile_find("BENCH-r0"));
    CHECK(p!=NULL);battery_bus_init();CHECK(!battery_bus_activate()&&isolated);
    CHECK(!battery_bus_publish(0x20,(uint8_t[]){50,0,0,0},4));
    for(unsigned n=0;n<1000;n++){
        wanted=(uint8_t)(n%3==0?25:n%3==1?50:100);
        CHECK(battery_bus_prepare(p,wanted)&&isolated&&!battery_bus_running());
        CHECK(battery_bus_activate()&&battery_bus_running()&&!isolated);
        battery_bus_stop();CHECK(isolated&&!battery_bus_running()&&battery_bus_engine()->prof==NULL);
    }
    unsigned before=enabled_calls;
    CHECK(!battery_bus_prepare(NULL,50)&&!battery_bus_activate()&&isolated);
    CHECK(!battery_bus_prepare(profile_get(profile_find("P12PM-r0")),50)&&!battery_bus_activate()&&isolated);
    CHECK(!battery_bus_prepare(p,17)&&!battery_bus_activate()&&isolated);
    CHECK(enabled_calls==before);
    p=profile_get(profile_find("SMB-BENCH"));
    for(unsigned n=0;n<1000;n++){
        wanted=(uint8_t)(n%3==0?25:n%3==1?50:100);
        CHECK(battery_bus_prepare(p,wanted)&&isolated);
        CHECK(battery_bus_activate()&&!isolated&&battery_bus_running());
        if(n==0){battery_bus_service(0);CHECK(battery_bus_engine()->overflows>0);battery_bus_clear_counters();battery_bus_service(1);CHECK(battery_bus_engine()->overflows==0&&battery_bus_engine()->txns==0);}
        battery_bus_stop();CHECK(isolated&&!battery_bus_running());
    }
    live_fault=true;before=enabled_calls;
    CHECK(!battery_bus_prepare(p,50)&&!battery_bus_activate()&&isolated);
    CHECK(enabled_calls==before);battery_bus_reset_fault();CHECK(!battery_bus_fault());
    r105_tests();
    printf("HAL event trace: %u checks, %u failures; %u activation/early-request cycles; output isolated on failures\n",checks,fails,enabled_calls);
    return fails?1:0;
}
