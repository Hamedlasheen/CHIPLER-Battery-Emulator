/* 062: SOC end to end for the iPhone-12-family DATA TEST backend (shared 12 Pro Max T06 backend).
 * Chain under test: soc_apply -> profile soc_words -> publish (WORDS marker) -> live_publish_soc -> SMBus read of 0x0D/0x0F with PEC.
 * Evidence-bound: only 0x0D and 0x0F move; every other register keeps the recorded T06 bytes. Result is "response data updated",
 * never "phone confirmed". */
#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_prepare.h"
#include "../../src/batt/soc_ctrl.h"
#include "../../src/batt/owner_words.h"
#include "owner_policy.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, fails, enters, masked;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static uint32_t guard(void *p){(void)p;unsigned o=masked;masked=1;enters++;return o;}
static void unguard(void *p,uint32_t v){(void)p;C(masked==1);masked=v;}
static st_target_t g_t;
/* default build: soc_words image published as is. OWNER build: the same rescale on the live 0x10 that main.c
 * publish_to_engine() applies (owner_rescale_words), so this suite covers soc_apply -> main publish -> engine -> PEC. */
static bool pub(void *ctx,uint8_t base,const uint8_t *d,uint8_t len){
    (void)ctx;
    if(OWNER&&base==SOC_PUBLISH_WORDS&&len<=12){
        uint8_t img[12];uint16_t fcc;memcpy(img,d,len);
        if(!st_get_word(&g_t,0x10,&fcc))return false;    /* main.c: battery_bus_word(0x10) = guarded st_get_word */
        (void)owner_rescale_words(img,len,fcc);return live_publish_soc(&g_t,base,img,len);
    }
    return live_publish_soc(&g_t,base,d,len);
}
/* recorded dataset served after profile load: default = shared T06 (0x0D 51 %, FCC 2913, 22 readable registers);
 * OWNER = 12 Pro capture rows (0x0D 2 %, FCC 2201, 44 readable registers) */
#define REC_SOC (OWNER?2u:51u)
#define REC_FCC (OWNER?2201u:2913u)
#define REC_NREG (OWNER?44u:22u)
/* independent reference: bitwise CRC-8 (poly 0x07, init 0, MSB first), NOT st_pec_read / any production CRC code */
static uint8_t ref_crc8(const uint8_t *p, unsigned n) {
    uint8_t c = 0;
    for (unsigned i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1)); }
    return c;
}
typedef struct { uint8_t len, data[32]; bool block; } frame_t;
/* one complete read transaction of a WORD or BLOCK register; PEC checked against ref_crc8 over 16 cmd 17 [count] data */
static bool rd_any(st_target_t *t, uint8_t cmd, bool block, frame_t *f) {
    uint8_t b;
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return false;
    f->block = block; f->len = 2;
    uint8_t pre[3 + 1 + 32]; unsigned n = 0; pre[n++] = 0x16; pre[n++] = cmd; pre[n++] = 0x17;
    if (block) { if (!st_on_tx(t, &b)) return false; f->len = b; pre[n++] = b; }
    for (unsigned k = 0; k < f->len; k++) { if (!st_on_tx(t, &f->data[k])) return false; pre[n++] = f->data[k]; }
    uint8_t pec; if (!st_on_tx(t, &pec)) return false;
    st_on_nack(t); if (st_on_stop(t) != ST_OK) return false;
    return pec == ref_crc8(pre, n);
}
static bool rd_word(st_target_t *t, uint8_t cmd, uint16_t *v) {
    frame_t f; if (!rd_any(t, cmd, false, &f)) return false; *v = (uint16_t)(f.data[0] | (f.data[1] << 8)); return true;
}
static const char *ids[4]={"P12m-B2","P12-B2","P12P-B2","P12PM-B2"};
int main(void){
    for(int m=0;m<4;m++){
        const batt_profile_t *p=profile_get(profile_find(ids[m]));C(p&&p->soc_words&&!p->soc_build&&p->status==PROF_PHONE_DATA_TEST);
        st_init(&g_t,0x0B);C(st_set_guard(&g_t,guard,unguard,0));C(live_prepare(&g_t,p,50)&&st_live_ready(&g_t));
        soc_ctrl_t s;soc_init(&s);
        /* profile load: recorded T06 51 % must stay; nothing silently applied (no default 50 over 51) */
        C(soc_on_profile_change(&s,p,pub,0)==SOC_RES_RECORDED_RETAINED&&s.applied==-1);
        uint16_t v;C(rd_word(&g_t,0x0D,&v)&&v==REC_SOC);
        frame_t before[ST_MAX_REGS];unsigned nreg=0;
        for(unsigned k=0;k<g_t.n_regs;k++){if(g_t.regs[k].shape==ST_SHAPE_WRITE)continue;C(rd_any(&g_t,g_t.regs[k].cmd,g_t.regs[k].shape==ST_SHAPE_BLOCK,&before[k]));nreg++;}
        C(nreg==REC_NREG);                                     /* every readable recorded register (WORD and BLOCK) is checked */
        static const int pct[3]={25,50,100};const uint16_t want_f[3]={OWNER?owner_remaining(2201,25):728,OWNER?owner_remaining(2201,50):1457,OWNER?owner_remaining(2201,100):2913};
        for(int i=0;i<3;i++){
            unsigned e0=enters;
            C(soc_apply(&s,p,pct[i],pub,0)==SOC_RES_APPLIED_PHONE_DATA&&s.applied==pct[i]);
            C(enters-e0==1);                                   /* both words updated under ONE guard (all-or-nothing map update; says nothing about two separate reads) */
            uint16_t d,f,fcc;C(rd_word(&g_t,0x0D,&d)&&rd_word(&g_t,0x0F,&f)&&rd_word(&g_t,0x10,&fcc));
            C(d==pct[i]&&f==want_f[i]&&fcc==REC_FCC);
            C((100u*f+fcc/2)/fcc==(unsigned)pct[i]);           /* recorded relation 0x0D = round(100*0x0F/0x10) holds on the served pair */
            for(unsigned k=0;k<g_t.n_regs;k++){                /* nothing else moved: every other WORD/BLOCK keeps its bytes (and PEC) */
                if(g_t.regs[k].shape==ST_SHAPE_WRITE||g_t.regs[k].cmd==0x0D||g_t.regs[k].cmd==0x0F)continue;
                frame_t x;C(rd_any(&g_t,g_t.regs[k].cmd,g_t.regs[k].shape==ST_SHAPE_BLOCK,&x));
                C(x.len==before[k].len&&!memcmp(x.data,before[k].data,x.len));
            }
        }
        uint16_t d;
        if(OWNER){                                            /* owner r7: any integer 1..100, 0 and 101 rejected */
            C(soc_apply(&s,p,42,pub,0)==SOC_RES_APPLIED_PHONE_DATA);C(rd_word(&g_t,0x0D,&d)&&d==42);
            uint16_t f;C(rd_word(&g_t,0x0F,&f)&&f==owner_remaining(2201,42)&&100u*f/2201u==42u);
            C(soc_apply(&s,p,0,pub,0)==SOC_RES_REJECTED&&soc_apply(&s,p,101,pub,0)==SOC_RES_REJECTED);
            C(rd_word(&g_t,0x0D,&d)&&d==42);                  /* rejected values changed nothing */
        }else{
            C(soc_apply(&s,p,42,pub,0)==SOC_RES_REJECTED);    /* only 25/50/100 */
            C(rd_word(&g_t,0x0D,&d)&&d==100);                 /* rejected value changed nothing */
        }
        /* a second profile change returns to the recorded dataset */
        st_init(&g_t,0x0B);C(st_set_guard(&g_t,guard,unguard,0));C(live_prepare(&g_t,p,50));C(rd_word(&g_t,0x0D,&d)&&d==REC_SOC);
    }
    /* bench SOC path is untouched, locked phone profiles refuse, WORDS marker is refused on the bench/locked target */
    const batt_profile_t *b=profile_get(profile_find("SMB-BENCH"));st_init(&g_t,0x0B);C(st_set_guard(&g_t,guard,unguard,0));C(live_prepare(&g_t,b,25));
    soc_ctrl_t s;soc_init(&s);C(soc_apply(&s,b,100,pub,0)==SOC_RES_APPLIED_SYNTHETIC);
    uint8_t trip[3]={0x0D,0x64,0};C(!live_publish_soc(&g_t,SOC_PUBLISH_WORDS,trip,3));
    const batt_profile_t *l=profile_get(profile_find(OWNER?"P15-r0":"P13-r0"));soc_init(&s);C(soc_apply(&s,l,50,pub,0)==SOC_RES_NOT_SUPPORTED);   /* review 106: P13 is shared in the owner build */
    /* malformed WORDS payloads on a phone target are refused whole (unknown cmd, wrong length, non-WORD) */
    st_init(&g_t,0x0B);C(st_set_guard(&g_t,guard,unguard,0));C(live_prepare(&g_t,profile_get(profile_find("P12PM-B2")),50));
    uint8_t bad1[6]={0x0D,0x19,0,0xEE,1,0},bad2[3]={0x66,1,0},bad3[2]={0x0D,1};
    C(!live_publish_soc(&g_t,SOC_PUBLISH_WORDS,bad1,6)&&!live_publish_soc(&g_t,SOC_PUBLISH_WORDS,bad2,3)&&!live_publish_soc(&g_t,SOC_PUBLISH_WORDS,bad3,2));
    uint16_t d;C(rd_word(&g_t,0x0D,&d)&&d==REC_SOC);           /* the valid first triplet of bad1 was NOT applied (all-or-nothing) */
    /* 063 R3: the guarantee is (1) the map update is all-or-nothing and (2) a response already started keeps its payload and PEC.
     * It is NOT a common generation across two separate command reads (no session lock is invented). */
    {
        const batt_profile_t *p=profile_get(profile_find("P12PM-B2"));
        st_init(&g_t,0x0B);C(st_set_guard(&g_t,guard,unguard,0));C(live_prepare(&g_t,p,50));
        soc_ctrl_t sc;soc_init(&sc);C(soc_apply(&sc,p,25,pub,0)==SOC_RES_APPLIED_PHONE_DATA);
        uint8_t b,lo,hi,pec;
        C(st_on_rx(&g_t,0x0D)&&st_begin_read(&g_t)&&st_on_tx(&g_t,&lo));          /* response in progress (first byte sent: 25) */
        C(soc_apply(&sc,p,100,pub,0)==SOC_RES_APPLIED_PHONE_DATA);                  /* update arrives mid-response */
        C(st_on_tx(&g_t,&hi)&&st_on_tx(&g_t,&pec));st_on_nack(&g_t);C(st_on_stop(&g_t)==ST_OK);
        {uint8_t d[3]={0x16,0x0D,0x17},w[5]={0x16,0x0D,0x17,lo,hi};(void)d;C(lo==25&&hi==0&&pec==ref_crc8(w,5));}   /* old complete frame, old PEC */
        uint16_t v;C(rd_word(&g_t,0x0D,&v)&&v==100);                                 /* the next request sees the update */
        /* characterisation (documented limitation, not a guarantee): two separate commands may come from different generations */
        soc_apply(&sc,p,25,pub,0);uint16_t d0,f0;C(rd_word(&g_t,0x0D,&d0)&&d0==25);
        soc_apply(&sc,p,100,pub,0);C(rd_word(&g_t,0x0F,&f0)&&f0==REC_FCC);            /* 0x0D=25 and 0x0F=2913 observed across two transactions */
        (void)b;
    }
    printf("soc_phone: %u checks, %u failures\n",checks,fails);return fails!=0;
}
