/* Review 106: every phone model outside families 15/16/17 runs on the working 12 Pro engine (owner build).
 *
 * Oracle: the owner's model list of addition 106 (written out below, independent of profile.c) and the 12 Pro capture
 * reference P12P-C1, whose served bytes are checked against the ORIGINAL battery recording by full_replay_tests
 * (replay_p12). Each target must arm exactly like that reference: same register map, same wire bytes and PEC, same
 * selector pages, SOC 1..100 on the map's own 0x10, picked from the UI with its own index, saved/restored under its own
 * id. Families 15/16/17, bench, offline-capture and internal profiles must keep their old contract.
 * Covered: profile registry, live_setup/live_prepare/smbus_target (production), owner_words, soc_ctrl, ui_nav, owner_state.
 * NOT covered here: main.c (glue, not built on host) and the IRQ path (bus_order_tests: first read after activation for
 * every target; full_replay_tests: IRQ/FIFO replay of P12PM-B2). No phone result is implied for any model. */
#include "../../src/batt/profile.h"
#include "../../src/batt/live_setup.h"
#include "../../src/batt/live_prepare.h"
#include "../../src/batt/owner_words.h"
#include "../../src/batt/soc_ctrl.h"
#include "../../src/ui/ui_nav.h"
#include "../../src/settings/owner_state.h"
#include "owner_policy.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, fails;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static unsigned masked;
static uint32_t guard(void *p){(void)p;unsigned o=masked;masked=1;return o;}
static void unguard(void *p,uint32_t v){(void)p;masked=v;}
static uint8_t crc8(const uint8_t *p,unsigned n){uint8_t c=0;for(unsigned i=0;i<n;i++){c^=p[i];for(int b=0;b<8;b++)c=(uint8_t)((c&0x80)?(c<<1)^0x07:(c<<1));}return c;}

/* addition 106, owner's list (X, XS/XS Max, XR, 11, 12, 13, 14 families; no SE exists in the registry) */
static const char *const TARGETS[] = {"PX-r0","PXS-r0","PXSM-r0","PXR-r0","P11-r0","P11P-r0","P11PM-r0",
    "P12m-B2","P12-B2","P12P-B2","P12PM-B2","P13m-r0","P13-r0","P13P-r0","P13PM-r0","P14-r0","P14Pl-r0","P14P-r0","P14PM-r0"};
#define NT (sizeof TARGETS / sizeof TARGETS[0])
static const char *const EXCLUDED[] = {"P15-r0","P15Pl-r0","P15P-r0","P15PM-r0","P16-r0","P16Pl-r0","P16P-r0","P16PM-r0",
    "P16e-r0","P17-r0","PAir-r0","P17P-r0","P17PM-r0"};
#define NX (sizeof EXCLUDED / sizeof EXCLUDED[0])
static const char *const UNCHANGED[] = {"BENCH-r0","SMB-BENCH","CAP12PM-B1","P12PM-r0"};   /* bench / offline / internal */
static bool in_list(const char *id, const char *const *l, unsigned n){for(unsigned k=0;k<n;k++)if(!strcmp(l[k],id))return true;return false;}

static bool arm(st_target_t *t, live_setup_t *s, const batt_profile_t *p, int soc){
    st_init(t,0x0B); st_set_guard(t,guard,unguard,0); return live_setup_prepare(s,t,p,soc,0);
}
static bool wire_word(st_target_t *t,uint8_t cmd,uint16_t *v){
    uint8_t d[2],pec,pre[5]={0x16,cmd,0x17};
    if(!st_on_rx(t,cmd)||!st_begin_read(t)||!st_on_tx(t,&d[0])||!st_on_tx(t,&d[1])||!st_on_tx(t,&pec))return false;
    st_on_nack(t);if(st_on_stop(t)!=ST_OK)return false;pre[3]=d[0];pre[4]=d[1];*v=(uint16_t)(d[0]|d[1]<<8);return pec==crc8(pre,5);
}
/* full block read on the wire; returns the byte count (incl. count byte) or -1 */
static int wire_block(st_target_t *t,uint8_t cmd,uint8_t *out){
    uint8_t pre[40]={0x16,cmd,0x17},n=3,cnt,b,pec;
    if(!st_on_rx(t,cmd)||!st_begin_read(t)||!st_on_tx(t,&cnt)||cnt>32)return -1;
    pre[n++]=cnt;out[0]=cnt;
    for(unsigned k=0;k<cnt;k++){if(!st_on_tx(t,&b))return -1;pre[n++]=b;out[1+k]=b;}
    if(!st_on_tx(t,&pec))return -1;st_on_nack(t);if(st_on_stop(t)!=ST_OK)return -1;
    return pec==crc8(pre,n)?1+cnt:-1;
}
static bool wire_write(st_target_t *t,uint8_t cmd,uint8_t b0,uint8_t b1){
    uint8_t body[2]={b0,b1};uint8_t pec=st_pec_write(0x0B,cmd,body,2);
    return st_on_rx(t,cmd)&&st_on_rx(t,b0)&&st_on_rx(t,b1)&&st_on_rx(t,pec)&&st_on_stop(t)==ST_OK;
}
static bool g_pub_fail;static st_target_t *g_t;
static bool w_word(void *c,uint8_t cmd,uint16_t *v){(void)c;return st_get_word(g_t,cmd,v);}
static bool w_pub(void *c,uint8_t b,const uint8_t *d,uint8_t n){(void)c;return !g_pub_fail&&live_publish_soc(g_t,b,d,n);}
/* main.c publish_to_engine (owner): rescale 0x0F on the live 0x10 inside the same atomic publish */
static bool soc_pub(void *c,uint8_t base,const uint8_t *d,uint8_t len){
    (void)c;uint8_t img[12];uint16_t fcc;if(len>12||!st_get_word(g_t,0x10,&fcc))return false;
    memcpy(img,d,len);(void)owner_rescale_words(img,len,fcc);return live_publish_soc(g_t,base,img,len);
}

static void inventory(void){
    unsigned nt=0,nx=0,nu=0,other=0;
    printf("| # | id | model | gen | scope | backend | status | armable(owner build=%d) |\n|---|---|---|---|---|---|---|---|\n",OWNER);
    for(int i=0;i<profile_count();i++){
        const batt_profile_t *p=profile_get(i);
        const char *scope=in_list(p->id,TARGETS,NT)?"TARGET 106":in_list(p->id,EXCLUDED,NX)?"EXCLUDED 15/16/17":
                          in_list(p->id,UNCHANGED,4)?"bench/offline/internal":"12 Pro capture reference";
        printf("| %d | %s | %s | %u | %s | %s | %s | %d |\n",i,p->id,p->name,p->gen,scope,profile_backend_str(p),profile_status_str(p->status),profile_armable(p));
        if(in_list(p->id,TARGETS,NT))nt++;else if(in_list(p->id,EXCLUDED,NX))nx++;else if(in_list(p->id,UNCHANGED,4))nu++;else other++;
    }
    /* the registry holds exactly the owner's list + exclusions + 4 bench/internal + the 12 Pro references */
    C(nt==NT&&nx==NX&&nu==4&&other==(unsigned)(2*CHIPLER_12P_REFERENCE));
    C(profile_count()==(int)(NT+NX+4+2*CHIPLER_12P_REFERENCE));
    printf("registry: %d profiles = %u targets + %u excluded + %u bench/internal + %u references\n",profile_count(),nt,nx,nu,other);
}

static void targets(void){
#if CHIPLER_12P_REFERENCE
    const batt_profile_t *ref=profile_get(profile_find("P12P-C1"));
    static st_target_t R;static live_setup_t RS;C(ref&&arm(&R,&RS,ref,50));
#endif
    for(unsigned k=0;k<NT;k++){
        int idx=profile_find(TARGETS[k]);const batt_profile_t *p=profile_get(idx);
        C(p!=0);if(!p)continue;
        C(p->gen>=10&&p->gen<=14&&!profile_family_excluded(p));          /* identity: its own family, not the 12 Pro's */
        C(p->status!=PROF_VERIFIED);                                       /* never VERIFIED from shared code */
        if(!OWNER){   /* default build: policy unchanged (only the 12 family keeps its T06 DATA TEST entries) */
            C(p->backend==BK_NONE&&profile_12p_capture_state(p)==0&&!profile_armable(p));
            C(profile_bus_wanted(p,false)==(p->status==PROF_PHONE_DATA_TEST));
            continue;
        }
        C(p->backend==BK_SHARED_12PRO&&!strcmp(profile_backend_str(p),"SHARED_12PRO"));
        C(profile_armable(p)&&profile_uses_12pm_backend(p)&&profile_12p_capture_state(p)==1&&p->soc_words);
        C(profile_bus_wanted(p,false)&&profile_restore_index(p->id)==idx);
#if CHIPLER_12P_REFERENCE
        /* engine: armed by the production setup, map identical to the replay-validated 12 Pro reference */
        static st_target_t T;static live_setup_t S;bool ev=false;
        st_init(&T,0x0B);st_set_guard(&T,guard,unguard,0);
        C(live_setup_prepare(&S,&T,p,50,&ev)&&ev&&st_live_ready(&T));
        C(T.n_regs==R.n_regs);
        for(unsigned r=0;r<T.n_regs&&r<R.n_regs;r++)
            C(T.regs[r].cmd==R.regs[r].cmd&&T.regs[r].shape==R.regs[r].shape&&T.regs[r].len==R.regs[r].len&&!memcmp(T.regs[r].data,R.regs[r].data,T.regs[r].len));
        /* wire: WORDs, a BLOCK, a count probe and a selector page, PEC checked independently, equal to the reference */
        static const uint8_t words[]={0x09,0x08,0x0D,0x0F,0x10,0x18};
        for(unsigned w=0;w<sizeof words;w++){uint16_t a,b;C(wire_word(&T,words[w],&a)&&wire_word(&R,words[w],&b)&&a==b);}
        uint8_t ba[40],bb[40];int na=wire_block(&T,0xA8,ba),nb=wire_block(&R,0xA8,bb);C(na>0&&na==nb&&!memcmp(ba,bb,(size_t)na));
        uint8_t cnt;C(st_on_rx(&T,0x7E)&&st_begin_read(&T)&&st_on_tx(&T,&cnt));st_on_nack(&T);C(st_on_stop(&T)==RD_PROBE(ST_SHORT_A8));
        C(wire_write(&T,0x65,0x01,0x00)&&wire_write(&R,0x65,0x01,0x00));
        na=wire_block(&T,0x66,ba);nb=wire_block(&R,0x66,bb);C(na==33&&na==nb&&!memcmp(ba,bb,33));
        /* SOC: initial words (R1 path) and the UI apply path (soc_apply + owner rescale), every value 1..100 */
        g_t=&T;const owner_bus_t ob={w_word,w_pub,0};uint16_t fcc=0,v;C(st_get_word(&T,0x10,&fcc));
        unsigned soc_bad=0;
        for(int s=1;s<=100;s++){
            if(!owner_initial_words(&ob,s,true,3.85f)||!wire_word(&T,0x0D,&v)||v!=s||!wire_word(&T,0x0F,&v)||v!=owner_remaining(fcc,(unsigned)s))soc_bad++;
            soc_ctrl_t sc;soc_init(&sc);
            if(soc_apply(&sc,p,s,soc_pub,0)!=SOC_RES_APPLIED_PHONE_DATA||sc.applied!=s||!wire_word(&T,0x0D,&v)||v!=s)soc_bad++;
        }
        C(soc_bad==0);C(!owner_initial_words(&ob,0,true,3.85f)&&!owner_initial_words(&ob,101,true,3.85f));
#endif
        /* UI: the model list row of THIS index opens the confirm for THIS index; APPLY loads it */
        ui_nav_t n;int arg=-1;const char *toast=0;nav_ctx_t c={OUT_OFF,profile_replies_off_index(),50,0,false};
        nav_init(&n,profile_count());n.screen=SCR_MODELS;
        nav_select(&n,idx+1,&c,&arg,&toast);
        C(n.screen==SCR_CONFIRM&&n.confirm==CONF_MODEL&&n.confirm_arg==idx);
        n.confirm_cur=1;C(nav_handle(&n,BTN_EV_OK,&c,&arg,&toast)==ACT_LOAD_PROFILE&&arg==idx);
        /* storage: saved and restored under its own id */
        owner_state_t st={{0},37,5},back;snprintf(st.prof,sizeof st.prof,"%s",p->id);
        uint8_t rec[OWNER_STATE_REC];owner_state_pack(&st,rec);
        C(owner_state_unpack(rec,&back)&&!strcmp(back.prof,p->id)&&back.soc==37&&profile_restore_index(back.prof)==idx);
    }
    printf("targets: %u models checked (%s)\n",(unsigned)NT,OWNER?"owner: shared 12 Pro engine":"default: unchanged");
}

static void negatives(void){
    for(unsigned k=0;k<NX;k++){   /* families 15/16/17: explicit family data, every build */
        const batt_profile_t *p=profile_get(profile_find(EXCLUDED[k]));C(p!=0);if(!p)continue;
        C(p->gen>=15&&p->gen<=17&&profile_family_excluded(p));
        C(p->backend==BK_NONE&&p->status==PROF_UNVERIFIED_LOCKED&&!profile_armable(p)&&!profile_uses_12pm_backend(p));
        C(profile_12p_capture_state(p)==0&&!profile_bus_wanted(p,false)&&profile_restore_index(p->id)==-1);
        st_target_t t;live_setup_t s;C(!arm(&t,&s,p,50));
    }
    /* the exclusion is the family field, not the id text: a hypothetical row marked SHARED in family 15 is still refused */
    batt_profile_t fake=*profile_get(profile_find("P14-r0"));fake.gen=15;fake.backend=BK_SHARED_12PRO;
    C(profile_family_excluded(&fake)&&profile_12p_capture_state(&fake)==0);
    /* bench / offline / internal: old contract */
    const batt_profile_t *b=profile_get(profile_find("BENCH-r0")),*sb=profile_get(profile_find("SMB-BENCH")),
        *cap=profile_get(profile_find("CAP12PM-B1")),*off=profile_get(profile_replies_off_index());
    C(b->status==PROF_BENCH_TEST&&sb->status==PROF_BENCH_TEST&&cap->status==PROF_BENCH_TEST);
    C(b->backend==BK_NONE&&sb->backend==BK_NONE&&cap->backend==BK_NONE&&!profile_armable(cap)&&profile_12p_capture_state(cap)==0);
    st_target_t t;live_setup_t s;C(arm(&t,&s,cap,50)&&t.n_regs==27);   /* offline capture bench: T06 catalog as before */
    C(off&&!strcmp(off->id,"P12PM-r0")&&off->gen==0&&off->status==PROF_IN_DEVELOPMENT&&off->backend==BK_NONE);
    C(!profile_armable(off)&&!profile_bus_wanted(off,false)&&!arm(&t,&s,off,50)&&profile_restore_index(off->id)==-1);
#if CHIPLER_12P_REFERENCE
    const batt_profile_t *c2=profile_get(profile_find("P12P-C2"));
    C(c2->backend==BK_NONE&&profile_12p_capture_state(c2)==2);   /* charger reference unchanged */
#endif
    printf("negatives: %u excluded models + bench/offline/internal + charger reference unchanged\n",(unsigned)NX);
}

/* STOP / serial 'x' / saved none, after every target. main.c: STOP and 'x' select profile_replies_off_index() and save ""
 * at once (replies are off: no 1.5 s debounce); boot restores via profile_restore_index(). Here: the bus policy input and
 * the restore decision for that state, and what the flash log returns after a power cut. */
static uint8_t g_sec[OWNER_STATE_SECTOR];
static void append(const owner_state_t *s){owner_job_t j;if(owner_state_plan(g_sec,s,false,&j)==OWNER_PLAN_WRITE){
    if(j.erase)memset(g_sec,0xFF,sizeof g_sec);for(unsigned k=0;k<OWNER_STATE_PAGE;k++)g_sec[j.off+k]&=j.page[k];}}
static void stop_none(void){
    if(!OWNER){printf("stop/none: owner build only\n");return;}
    const batt_profile_t *off=profile_get(profile_replies_off_index());
    for(unsigned k=0;k<NT;k++){
        memset(g_sec,0xFF,sizeof g_sec);
        owner_state_t a={{0},55,0};snprintf(a.prof,sizeof a.prof,"%s",TARGETS[k]);append(&a);
        owner_state_t got;int nf;C(owner_state_scan(g_sec,sizeof g_sec,&got,&nf)&&profile_restore_index(got.prof)==profile_find(TARGETS[k]));
        /* STOP: replies-off reference selected -> the next control tick does not start the target (output OFF) */
        C(!profile_bus_wanted(off,false));
        owner_state_t none={{0},55,0};append(&none);
        C(owner_state_scan(g_sec,sizeof g_sec,&got,&nf)&&got.prof[0]==0&&profile_restore_index(got.prof)==-1);   /* after restart: off */
        C(owner_state_boot(g_sec,sizeof g_sec,&got)==OWNER_BOOT_NONE);
        /* review 110 R110-1: power cut while the "none" record is programmed: its CRC fails -> the boot is UNCERTAIN,
         * the previous record (this model) is NOT offered and nothing starts */
        unsigned off_rec=(unsigned)(nf>0?nf-1:OWNER_STATE_SECTOR/OWNER_STATE_REC-1)*OWNER_STATE_REC;
        g_sec[off_rec+28]^=0xFF;   /* torn crc */
        C(owner_state_boot(g_sec,sizeof g_sec,&got)==OWNER_BOOT_UNCERTAIN);
    }
    printf("stop/none: %u targets -> replies-off reference, saved none restores nothing; torn none record -> UNCERTAIN, model not offered\n",(unsigned)NT);
}

/* review 110 R110-2: who can arm the output, per build, against the independent lists above; negative controls prove the
 * check notices an unintended unlock (a DATA TEST row in an excluded family, any non-phone status). */
static void output_policy(void){
    unsigned armed=0;
    for(int i=0;i<profile_count();i++){
        const batt_profile_t *p=profile_get(i);
        bool want=OWNER&&(in_list(p->id,TARGETS,NT)||(CHIPLER_12P_REFERENCE&&(!strcmp(p->id,"P12P-C1")||!strcmp(p->id,"P12P-C2"))));
        if(profile_armable(p)!=want)printf("  policy mismatch %s armable=%d expected=%d\n",p->id,profile_armable(p),want);
        C(profile_armable(p)==want);armed+=profile_armable(p);
    }
    C(armed==(OWNER?NT+2u*CHIPLER_12P_REFERENCE:0u));
    batt_profile_t f=*profile_get(profile_find("P15-r0"));f.status=PROF_PHONE_DATA_TEST;C(!profile_armable(&f));   /* excluded family */
    f=*profile_get(profile_find("P14-r0"));f.status=PROF_PHONE_DATA_TEST;C(profile_armable(&f)==(bool)OWNER);       /* default: never */
    f.status=PROF_BENCH_TEST;C(!profile_armable(&f));f.status=PROF_IN_DEVELOPMENT;C(!profile_armable(&f));f.status=PROF_UNVERIFIED_LOCKED;C(!profile_armable(&f));
    f.status=PROF_VERIFIED;C(profile_armable(&f));   /* the only status every build accepts (no registry row has it) */
    printf("output policy (%s build): %u armable, families 15/16/17/bench/internal never\n",OWNER?"owner":"default",armed);
}

int main(void){
    inventory(); targets(); negatives(); stop_none(); output_policy();
    printf("shared12: %u checks, %u failures: %s\n",checks,fails,fails?"FAIL":"PASS");
    return fails?1:0;
}
