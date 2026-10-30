/* All original decoded fixtures through the NEW engine, not only the legacy replay core. */
#include "../../src/batt/smbus_target.h"
#include <stdio.h>
#include <string.h>
typedef struct {uint8_t addr;uint16_t nw,nr;uint8_t flags,expected,w[35],r[35];} capture_frame_t;
#include "../../src/batt/capture_data.inc"
static unsigned checks,fails,reads,writes,shorts;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL frame=%u line=%d %s\n",i,__LINE__,#x);}}while(0)
int main(void){
 for(unsigned i=0;i<CAPTURE_FRAME_COUNT;i++){
  const capture_frame_t *f=&capture_frames[i];
  if(f->expected!=0&&f->expected!=1)continue;
  st_target_t t;st_init(&t,0x0B);
  if(f->flags&1){
   bool short_a8=f->expected==1,word=f->nr==3;
   unsigned n=short_a8?24:word?2:f->r[0];const uint8_t *payload=f->r+(word?0:1);
   C(st_add(&t,f->w[0],word?ST_SHAPE_WORD:ST_SHAPE_BLOCK,(uint8_t)n));
   if(word)C(st_set_word(&t,f->w[0],(uint16_t)(payload[0]|payload[1]<<8)));
   else {uint8_t empty[32]={0};C(st_set_block(&t,f->w[0],short_a8?empty:payload,(uint8_t)n));}
   C(st_seal(&t));C(st_on_rx(&t,f->w[0]));C(st_begin_read(&t));
   for(unsigned k=0;k<f->nr;k++){uint8_t b=0;C(st_on_tx(&t,&b));C(b==f->r[k]);}
   st_on_nack(&t);C(st_on_stop(&t)==(short_a8?ST_SHORT_A8:ST_OK));
   if(short_a8)shorts++;else reads++;
  }else{
   unsigned n=f->nw==4?2:f->w[1];C(st_add(&t,f->w[0],ST_SHAPE_WRITE,(uint8_t)n));C(st_seal(&t));
   for(unsigned k=0;k<f->nw;k++)C(st_on_rx(&t,f->w[k]));
   C(t.writes_ok==0);C(st_on_stop(&t)==ST_OK);st_write_t w;
   C(st_pop_write(&t,&w)&&w.cmd==f->w[0]&&w.len==f->nw-2);C(!memcmp(w.body,f->w+1,w.len));writes++;
  }
 }
 printf("NEW live engine original captures: %u checks, %u failures; reads=%u writes=%u shortA8=%u; opaque/rejected not promoted\n",checks,fails,reads,writes,shorts);
 return fails||reads!=3444||writes!=86||shorts!=19?1:0;
}
