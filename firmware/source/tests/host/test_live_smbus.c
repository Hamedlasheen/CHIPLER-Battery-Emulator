#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_wire.h"
#include "../../src/batt/live_prepare.h"
#include "smbus_vectors.inc"
#include <stdio.h>
#include "owner_policy.h"
#include <string.h>
#include <stdlib.h>
static unsigned checks,fails,masked,enters;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static uint32_t guard(void *p){(void)p;unsigned old=masked;masked=1;enters++;return old;}
static void unguard(void *p,uint32_t v){(void)p;C(masked==1);masked=v;}
static void start(st_target_t *t){st_init(t,0x0B);C(st_set_guard(t,guard,unguard,0));}
static void word(st_target_t *t){start(t);C(st_add(t,9,ST_SHAPE_WORD,2));C(st_set_word(t,9,0x1122));C(st_seal(t));}
static void command(st_target_t *t,uint8_t c){C(st_on_rx(t,c));C(st_begin_read(t));}
static unsigned read_word(st_target_t *t){uint8_t lo=0,hi=0,pec=0;C(st_on_tx(t,&lo));C(st_on_tx(t,&hi));C(st_on_tx(t,&pec));st_on_nack(t);C(st_on_stop(t)==ST_OK);return lo|((unsigned)hi<<8);}
static void golden(void){
 for(unsigned k=0;k<sizeof SMBUS_VECTORS/sizeof SMBUS_VECTORS[0];k++){
  const smbus_vector_t *v=&SMBUS_VECTORS[k];st_target_t t;start(&t);
  st_shape_t shape=v->shape==SV_WORD?ST_SHAPE_WORD:v->shape==SV_WRITE?ST_SHAPE_WRITE:ST_SHAPE_BLOCK;
  uint8_t n=v->shape==SV_A8?24:v->len;C(st_add(&t,v->cmd,shape,n));
  if(shape==ST_SHAPE_WORD)C(st_set_word(&t,v->cmd,(uint16_t)(v->payload[0]|v->payload[1]<<8)));
  else if(shape==ST_SHAPE_BLOCK){uint8_t p[32]={0};if(v->shape!=SV_A8)memcpy(p,v->payload,n);C(st_set_block(&t,v->cmd,p,n));}
  C(st_seal(&t));C(st_on_rx(&t,v->cmd));
  if(v->shape==SV_WRITE){
   for(unsigned j=0;j<v->len;j++)C(st_on_rx(&t,v->payload[j]));
   C(st_on_rx(&t,v->pec));
   C(t.writes_ok==0);C(st_on_stop(&t)==ST_OK);st_write_t w;C(st_pop_write(&t,&w));C(w.cmd==v->cmd&&w.len==v->len);C(!memcmp(w.body,v->payload,v->len));
  }else{
   C(st_begin_read(&t));uint8_t b=0;
   if(v->shape!=SV_WORD){C(st_on_tx(&t,&b));C(b==n);}
   if(v->shape!=SV_A8){for(unsigned j=0;j<v->len;j++){C(st_on_tx(&t,&b));C(b==v->payload[j]);}C(st_on_tx(&t,&b));C(b==v->pec);}
   st_on_nack(&t);C(st_on_stop(&t)==(v->shape==SV_A8?ST_SHORT_A8:ST_OK));
  }
 }
}
static void regressions(void){
 st_target_t t;word(&t);C(st_live_ready(&t));C(!st_add(&t,0xEE,ST_SHAPE_WORD,2));
 C(st_on_rx(&t,0xEE));uint8_t b=0x5A;
 if(OWNER){C(st_begin_read(&t)&&t.idle_fill);C(st_on_tx(&t,&b)&&b==0xFF);}  /* owner: unknown read = released-bus 0xFF */
 else{C(!st_begin_read(&t));C(!st_on_tx(&t,&b)&&b==0x5A);}
 st_abort(&t);
 C(!st_begin_read(&t));C(t.nw==0&&!t.active); /* no stale pointer after abort */
 command(&t,9);for(int k=0;k<3;k++)C(st_on_tx(&t,&b));C(!st_on_tx(&t,&b));st_on_nack(&t);C(st_on_stop(&t)==ST_OVERREAD);C(t.reads_done==0&&t.overreads==1);
 command(&t,9);C(st_on_tx(&t,&b));C(b==0x22);
 unsigned before=enters;C(st_set_word(&t,9,0x3344));C(enters==before+1&&masked==0);
 C(st_on_tx(&t,&b)&&b==0x11);C(st_on_tx(&t,&b));st_on_nack(&t);C(st_on_stop(&t)==ST_OK);
 command(&t,9);C(read_word(&t)==0x3344); /* whole read immutable despite control update */
 C(st_on_rx(&t,9));C(st_on_stop(&t)==ST_OK);C(!st_begin_read(&t)); /* STOP is not Sr */
 start(&t);C(st_add(&t,0x65,ST_SHAPE_WRITE,2));C(st_seal(&t));uint8_t p[2]={0,0};
 C(st_on_rx(&t,0x65));C(st_on_rx(&t,0));C(st_on_rx(&t,0));C(st_on_rx(&t,st_pec_write(0x0B,0x65,p,2)));
 C(!st_begin_read(&t));C(t.writes_ok==0); /* repeated START must not commit */
 st_on_restart_write(&t);C(t.writes_ok==0);
}
static void writes_and_bounds(void){
 st_target_t t;start(&t);C(st_add(&t,0x65,ST_SHAPE_WRITE,2));C(st_seal(&t));uint8_t p[2]={0x11,0x22};
 for(unsigned k=0;k<ST_WRITE_QUEUE+1;k++){
  C(st_on_rx(&t,0x65));C(st_on_rx(&t,p[0]));C(st_on_rx(&t,p[1]));C(st_on_rx(&t,st_pec_write(0x0B,0x65,p,2)));
  C(st_on_stop(&t)==(k<ST_WRITE_QUEUE?ST_OK:WR_ERR(ST_QUEUE_FULL)));
 }
 C(t.writes_ok==ST_WRITE_QUEUE&&t.queue_dropped==1);st_write_t w;for(unsigned k=0;k<ST_WRITE_QUEUE;k++)C(st_pop_write(&t,&w)&&w.body[0]==0x11);C(!st_pop_write(&t,&w));
 C(st_on_rx(&t,0x65));C(st_on_rx(&t,0x11));C(st_on_rx(&t,0x22));C(st_on_rx(&t,0xFF));C(st_on_stop(&t)==WR_ERR(ST_BAD_PEC));
 C(t.writes_ok==ST_WRITE_QUEUE);st_abort(&t);for(int k=0;k<35;k++)C(st_on_rx(&t,(uint8_t)k));C(!st_on_rx(&t,0));C(st_on_stop(&t)==WR_ERR(ST_BAD_LENGTH));
}
static void events(void){
 st_target_t t;live_wire_t w;live_reply_t r;word(&t);live_wire_init(&w,&t,0);
 live_event_t e={.rx={0x0809},.count=1,.request=true};C(live_wire_event(&w,&e,1,&r)&&r.have_tx&&r.tx==0x22);
 e=(live_event_t){.request=true};C(live_wire_event(&w,&e,2,&r)&&r.tx==0x11);C(live_wire_event(&w,&e,3,&r));
 e=(live_event_t){.nack=true,.stop=true};C(live_wire_event(&w,&e,4,&r));C(t.reads_done==1);
 e=(live_event_t){.request=true};C(live_wire_event(&w,&e,5,&r)&&!w.blocked&&r.have_tx&&r.tx==0x22); /* R3: pointer retained after a completed KNOWN read */
 word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.request=true};C(!live_wire_event(&w,&e,1,&r)&&w.blocked&&!r.have_tx); /* cold bus: no invented pointer */
 { /* pointer-only STOP: known command retained, unknown cleared, write clears, abort clears */
   word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.rx={0x0809},.count=1,.stop=true};C(live_wire_event(&w,&e,1,&r)&&t.ptr_valid&&t.ptr==0x09);
   e=(live_event_t){.request=true};C(live_wire_event(&w,&e,2,&r)&&r.have_tx&&r.tx==0x22);
   word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.rx={0x0877},.count=1,.stop=true};C(live_wire_event(&w,&e,1,&r)&&!t.ptr_valid);
   e=(live_event_t){.request=true};C(!live_wire_event(&w,&e,2,&r)&&!r.have_tx);
   word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.rx={0x0809},.count=1,.stop=true};C(live_wire_event(&w,&e,1,&r));st_abort(&t);C(!t.ptr_valid);
   e=(live_event_t){.request=true};C(!live_wire_event(&w,&e,2,&r)&&!r.have_tx);
 }
 word(&t);live_wire_init(&w,&t,UINT32_MAX-10);e=(live_event_t){.rx={0x0809},.count=1};C(live_wire_event(&w,&e,UINT32_MAX-5,&r));
 C(!live_wire_expire(&w,3,100));C(live_wire_expire(&w,200,100)&&w.blocked);C(!t.active&&t.nw==0);
 word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.rx={0x0809},.count=1,.stop=true};C(live_wire_event(&w,&e,1,&r));
 e=(live_event_t){.request=true,.stop=true};C(!live_wire_event(&w,&e,2,&r));
 word(&t);live_wire_init(&w,&t,0);e=(live_event_t){.rx={0x0809},.count=1};C(live_wire_event(&w,&e,1,&r));
 e=(live_event_t){.rx={0x0809},.count=1,.stop=true};C(!live_wire_event(&w,&e,2,&r)); /* ambiguous old STOP/new START */
 /* Last write bytes and STOP coalesced: drain FIFO before validating STOP. */
 start(&t);C(st_add(&t,0x65,ST_SHAPE_WRITE,2));C(st_seal(&t));live_wire_init(&w,&t,0);
 uint8_t p[2]={0,0};e=(live_event_t){.rx={0x0865,0,0,st_pec_write(0x0B,0x65,p,2)},.count=4,.stop=true};
 C(live_wire_event(&w,&e,1,&r));C(t.writes_ok==1);
}
static void phone_data_one(const char *id){
 /* ported from beta2: T06 single-session samples as an explicit PHONE DATA TEST profile (data only, never armable) */
 st_target_t t;start(&t);const batt_profile_t *p=profile_get(profile_find(id));
 /* owner build: DATA TEST profiles are armable and serve the 12 Pro capture rows (51 regs, 0x09=3455 mV) instead of T06 */
 C(p&&profile_uses_12pm_backend(p)&&p->status==PROF_PHONE_DATA_TEST&&profile_armable(p)==(bool)OWNER&&!p->soc_build);
 C(live_prepare(&t,p,50)&&st_live_ready(&t)&&t.n_regs==(OWNER?51u:27u));
 uint8_t d[4]={0};
 C(st_on_rx(&t,0x09)&&st_begin_read(&t));{uint8_t b[3];for(int k=0;k<3;k++)C(st_on_tx(&t,&b[k]));C(OWNER?(b[0]==0x7F&&b[1]==0x0D):(b[0]==0x46&&b[1]==0x0F));}
 st_on_nack(&t);C(st_on_stop(&t)==ST_OK);
 C(!live_publish_soc(&t,0x0D,d,4));
 C(st_on_rx(&t,0xEE));C(st_begin_read(&t)==(bool)OWNER);st_abort(&t);
 C(t.have_last_cmd&&t.last_cmd==0xEE&&t.nw==0&&!t.active);C(!st_begin_read(&t));
}
static void phone_data_profile(void){
 /* owner decision 060: iPhone 12 mini / 12 / 12 Pro / 12 Pro Max all select the SAME shared backend and responses; none is VERIFIED/armable */
 static const char *ids[4]={"P12m-B2","P12-B2","P12P-B2","P12PM-B2"};
 for(int i=0;i<4;i++)phone_data_one(ids[i]);
 for(int i=0;i<4;i++){const batt_profile_t *p=profile_get(profile_find(ids[i]));C(p&&p->i2c_addr==0x0B&&p->status!=PROF_VERIFIED&&profile_armable(p)==(bool)OWNER&&!p->regs);}
 /* families 15/16/17 and the replies-off reference stay locked and are refused by the engine in every build */
 st_target_t t;start(&t);C(!live_prepare(&t,profile_get(profile_find("P15-r0")),50));C(!live_prepare(&t,profile_get(profile_find("P12PM-r0")),50));
 C(!profile_uses_12pm_backend(profile_get(profile_find("P17PM-r0")))&&!profile_uses_12pm_backend(profile_get(profile_find("P16e-r0"))));
 /* review 106: X..14 models run on the shared 12 Pro engine in the owner build only; the default build keeps them locked */
 C(profile_uses_12pm_backend(profile_get(profile_find("P11-r0")))==(bool)OWNER&&profile_uses_12pm_backend(profile_get(profile_find("P13-r0")))==(bool)OWNER);
}
static void catalog_and_soc(void){
 st_target_t t;start(&t);const batt_profile_t *p=profile_get(profile_find("CAP12PM-B1"));
 C(live_prepare(&t,p,50)&&t.n_regs==27&&st_live_ready(&t));C(!p->soc_build&&!profile_armable(p));
 uint8_t d[4]={100,0,0,0};C(!live_publish_soc(&t,0x20,d,4));
 start(&t);p=profile_get(profile_find("SMB-BENCH"));C(live_prepare(&t,p,25));
 C(live_publish_soc(&t,0x20,d,4));command(&t,0x20);uint8_t b;C(st_on_tx(&t,&b)&&b==4);C(st_on_tx(&t,&b)&&b==100);st_abort(&t);
 start(&t);C(!live_prepare(&t,profile_get(profile_find("P12PM-r0")),50));C(!st_live_ready(&t));
}
static void random_transactions(void){
 uint32_t seed=0xA51C3u;unsigned good=0;
 for(unsigned k=0;k<10000;k++){
  seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
  st_target_t t;word(&t);uint16_t val=(uint16_t)seed;C(st_set_word(&t,9,val));command(&t,9);
  unsigned n=(seed>>16)%5;uint8_t b=0;
  for(unsigned j=0;j<n;j++){bool ok=st_on_tx(&t,&b);C(ok==(j<3));}
  if(seed&1)st_on_nack(&t);
  st_result_t r=st_on_stop(&t);bool exact=n==3&&(seed&1);C((r==ST_OK)==exact);good+=exact;
  C(!t.active&&t.nw==0&&t.tx_off==0);C(t.reads_done==(unsigned)exact);
 }
 printf("random complete/short/overread/NACK cases: 10000; accepted exact=%u\n",good);
}
int main(void){golden();regressions();writes_and_bounds();events();catalog_and_soc();phone_data_profile();random_transactions();
 printf("BETA1 live SMBus: %u checks, %u failures; 25 inherited captured vectors\n",checks,fails);return fails?1:0;}
