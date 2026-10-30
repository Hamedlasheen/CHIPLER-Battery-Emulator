/* OWNER r8: saved profile + SOC record log (settings/owner_state.c): pack/unpack, CRC, newest-wins scan, torn and
 * erased slots, full sector, sequence wrap. */
#include <stdio.h>
#include <string.h>
#include "../../src/settings/owner_state.h"
#include "../../src/settings/settings_store.h"

static unsigned fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define SECTOR 4096u
#define SLOTS (SECTOR / OWNER_STATE_REC)

static owner_state_t mk(const char *id, int soc, uint32_t seq) {
    owner_state_t s; memset(&s, 0, sizeof s); strncpy(s.prof, id, OWNER_STATE_ID_LEN); s.soc = soc; s.seq = seq; return s;
}
/* NOR program semantics: bits only go 1 -> 0 */
static void program(uint8_t *sec, unsigned slot, const owner_state_t *s) {
    uint8_t r[OWNER_STATE_REC]; owner_state_pack(s, r);
    for (unsigned i = 0; i < OWNER_STATE_REC; i++) sec[slot * OWNER_STATE_REC + i] &= r[i];
}


/* ---- fake NOR flash: erase -> 0xFF, program ANDs bits (1 -> 0 only), optional power cut after k bytes ---- */
static uint8_t nor[SECTOR];
static unsigned nor_erases, nor_programs;
static void nor_erase(void) { memset(nor, 0xFF, sizeof nor); nor_erases++; }
static void nor_program(uint32_t off, const uint8_t *page, unsigned cut) {   /* cut = bytes written before power loss */
    nor_programs++;
    for (unsigned i = 0; i < OWNER_STATE_PAGE && i < cut; i++) nor[off + i] &= page[i];
}
/* device sequence of owner_state_save(): plan, execute, read back */
static owner_plan_t save(const owner_state_t *s, bool bus_running, unsigned cut) {
    owner_job_t job;
    owner_plan_t pl = owner_state_plan(nor, s, bus_running, &job);
    if (pl == OWNER_PLAN_WRITE) { if (job.erase) nor_erase(); nor_program(job.off, job.page, cut); }
    return pl;
}
static bool load(owner_state_t *s) { int nf; return owner_state_scan(nor, SECTOR, s, &nf); }

static void integration(void) {
    owner_state_t b;
    memset(nor, 0xFF, sizeof nor); nor_erases = nor_programs = 0;
    CHECK(!load(&b));                                                         /* blank flash: nothing restored */
    /* 300 alternating saves with the bus stopped: always the newest, sector recycled by erase */
    for (int i = 0; i < 300; i++) {
        owner_state_t w = mk(i % 3 ? "P12P-C1" : "", i % 100 + 1, 0);
        CHECK(save(&w, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE);
        CHECK(load(&b) && owner_state_same(&b, &w) && b.seq == (uint32_t)i + 1);
    }
    CHECK(nor_erases == 300 / SLOTS && nor_programs == 300);
    /* identical request: no flash write */
    { unsigned np = nor_programs; owner_state_t w = b; CHECK(save(&w, false, OWNER_STATE_PAGE) == OWNER_PLAN_SAME && nor_programs == np); }
    /* fill the sector, then a save while the bus runs must NOT erase; the last record stays valid until STOP */
    memset(nor, 0xFF, sizeof nor);
    for (unsigned i = 0; i < SLOTS; i++) { owner_state_t w = mk("P12P-B2", (int)(i % 100) + 1, 0); save(&w, true, OWNER_STATE_PAGE); }
    owner_state_t full; CHECK(load(&full));
    { unsigned ne = nor_erases; owner_state_t w = mk("P12P-C1", 37, 0);
      CHECK(save(&w, true, OWNER_STATE_PAGE) == OWNER_PLAN_WAIT_ERASE && nor_erases == ne);
      CHECK(load(&b) && owner_state_same(&b, &full));
      CHECK(save(&w, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE && nor_erases == ne + 1);   /* after STOP */
      CHECK(load(&b) && owner_state_same(&b, &w) && b.seq == full.seq + 1); }
    /* power cut in the middle of every possible byte of a record: the previous state survives, next save works */
    for (unsigned cut = 0; cut < OWNER_STATE_REC; cut++) {
        memset(nor, 0xFF, sizeof nor);
        owner_state_t a = mk("P12P-C1", 40, 0), w = mk("P12-B2", 77, 0), z = mk("P12P-C1", 41, 0);
        save(&a, false, OWNER_STATE_PAGE);
        owner_job_t job; CHECK(owner_state_plan(nor, &w, false, &job) == OWNER_PLAN_WRITE);
        unsigned rec_off = 1 * OWNER_STATE_REC - job.off;                      /* the new record is slot 1 */
        nor_program(job.off, job.page, rec_off + cut);                          /* lose power after `cut` bytes */
        CHECK(load(&b) && owner_state_same(&b, &a));
        CHECK(save(&z, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE && load(&b) && owner_state_same(&b, &z));
    }
    /* power cut during an erase: garbage in the sector never restores a wrong record, the next save recovers */
    memset(nor, 0xA5, sizeof nor);
    CHECK(!load(&b));
    { owner_state_t w = mk("P12P-C1", 64, 0); CHECK(save(&w, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE && load(&b) && owner_state_same(&b, &w)); }
    /* a record of another storage version is ignored (no migration): state = none */
    memset(nor, 0xFF, sizeof nor);
    { owner_state_t w = mk("P12P-C1", 64, 1); uint8_t r[OWNER_STATE_REC]; owner_state_pack(&w, r); r[4] = 2;
      uint32_t c = settings_crc32(r, 28); r[28] = (uint8_t)c; r[29] = (uint8_t)(c >> 8); r[30] = (uint8_t)(c >> 16); r[31] = (uint8_t)(c >> 24);
      memcpy(nor, r, sizeof r); CHECK(!load(&b)); }
    /* region check: the two reserved sectors must be above the program image */
    CHECK(owner_state_region_ok(1u << 20, 2u << 20) && owner_state_region_ok((2u << 20) - 8192u, 2u << 20));
    CHECK(!owner_state_region_ok((2u << 20) - 8191u, 2u << 20) && !owner_state_region_ok(0, 4096u));
}
int main(void) {
    uint8_t rec[OWNER_STATE_REC];
    owner_state_t a = mk("P12P-C1", 37, 5), b;
    owner_state_pack(&a, rec);
    CHECK(owner_state_unpack(rec, &b) && !strcmp(b.prof, "P12P-C1") && b.soc == 37 && b.seq == 5);

    /* every single-bit flip is rejected */
    unsigned det = 0, tot = 0;
    for (unsigned i = 0; i < OWNER_STATE_REC; i++) for (unsigned k = 0; k < 8; k++) {
        uint8_t r2[OWNER_STATE_REC]; memcpy(r2, rec, sizeof r2); r2[i] ^= (uint8_t)(1u << k); tot++;
        if (!owner_state_unpack(r2, &b)) det++;
    }
    CHECK(det == tot);

    /* SOC out of range and empty profile */
    { owner_state_t z = mk("P12P-C1", 0, 1); owner_state_pack(&z, rec); CHECK(!owner_state_unpack(rec, &b));
      z = mk("P12P-C1", 101, 1); owner_state_pack(&z, rec); CHECK(!owner_state_unpack(rec, &b));
      z = mk("", 50, 1); owner_state_pack(&z, rec); CHECK(owner_state_unpack(rec, &b) && b.prof[0] == 0 && b.soc == 50); }

    uint8_t sec[SECTOR]; int nf;
    /* erased sector: nothing, first slot free */
    memset(sec, 0xFF, sizeof sec);
    CHECK(!owner_state_scan(sec, SECTOR, &b, &nf) && nf == 0);

    /* appended records: newest wins, next free follows the last written slot */
    owner_state_t r1 = mk("P12P-C1", 50, 1), r2 = mk("P12P-C1", 37, 2), r3 = mk("", 37, 3);
    program(sec, 0, &r1); program(sec, 1, &r2);
    CHECK(owner_state_scan(sec, SECTOR, &b, &nf) && b.soc == 37 && b.seq == 2 && nf == 2);
    program(sec, 2, &r3);
    CHECK(owner_state_scan(sec, SECTOR, &b, &nf) && b.prof[0] == 0 && b.seq == 3 && nf == 3);

    /* a torn record (power lost mid-program) is skipped; the slot counts as used */
    { uint8_t t[OWNER_STATE_REC]; owner_state_t r4 = mk("P12-B2", 80, 4); owner_state_pack(&r4, t);
      for (unsigned i = 16; i < OWNER_STATE_REC; i++) t[i] = 0xFF;
      memcpy(sec + 3 * OWNER_STATE_REC, t, OWNER_STATE_REC);
      CHECK(owner_state_scan(sec, SECTOR, &b, &nf) && b.seq == 3 && nf == 4); }

    /* full sector -> next_free -1, newest still found */
    memset(sec, 0xFF, sizeof sec);
    for (unsigned i = 0; i < SLOTS; i++) { owner_state_t r = mk("P12P-B2", (int)(i % 100) + 1, i + 1); program(sec, i, &r); }
    CHECK(owner_state_scan(sec, SECTOR, &b, &nf) && nf == -1 && b.seq == SLOTS && b.soc == (int)((SLOTS - 1) % 100) + 1);

    /* sequence wrap: 0xFFFFFFFF then 0 -> 0 is newer */
    memset(sec, 0xFF, sizeof sec);
    { owner_state_t x = mk("P12P-C1", 10, 0xFFFFFFFFu), y = mk("P12P-C1", 20, 0); program(sec, 0, &x); program(sec, 1, &y);
      CHECK(owner_state_scan(sec, SECTOR, &b, &nf) && b.soc == 20); }

    /* same() ignores seq */
    { owner_state_t x = mk("P12P-C1", 10, 1), y = mk("P12P-C1", 10, 9), z = mk("P12P-C2", 10, 1);
      CHECK(owner_state_same(&x, &y) && !owner_state_same(&x, &z)); }

    integration();
    /* review 110 R110-1: boot decision is fail-closed. A STOP ("none") that did not fully reach flash never brings the
     * previous profile back; a clean record is only OFFERED (main.c starts replies on APPLY, OWNER_AUTOSTART_AFTER_RESTORE 0). */
    {   owner_state_t b, model = mk("P13-r0", 64, 0), none = mk("", 64, 0);
        nor_erase();
        CHECK(owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_NONE);                          /* blank flash */
        CHECK(save(&model, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE);
        CHECK(owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_OFFER && !strcmp(b.prof, "P13-r0") && b.soc == 64);
        /* power cut at EVERY byte of the STOP record: never OFFER the old model again */
        unsigned offer = 0, cases = 0;
        for (unsigned cut = 0; cut <= OWNER_STATE_PAGE; cut++) {
            nor_erase(); save(&model, false, OWNER_STATE_PAGE);
            owner_job_t j; CHECK(owner_state_plan(nor, &none, false, &j) == OWNER_PLAN_WRITE);
            unsigned rec = (j.seq - 1) * OWNER_STATE_REC - j.off;   /* first byte of the new record inside the page */
            nor_program(j.off, j.page, cut);
            owner_boot_t r = owner_state_boot(nor, SECTOR, &b);
            if (cut <= rec) CHECK(r == OWNER_BOOT_OFFER);          /* no bit of the STOP reached flash: an event that never
                                                                       happened on the medium (documented, not hidden) */
            else if (cut >= rec + OWNER_STATE_REC) CHECK(r == OWNER_BOOT_NONE);
            else { cases++; offer += r == OWNER_BOOT_OFFER; CHECK(r == OWNER_BOOT_UNCERTAIN || r == OWNER_BOOT_NONE); }
        }
        CHECK(cases == OWNER_STATE_REC - 1 && offer == 0);
        /* the record bytes are partly 0xFF-valued: a cut inside them may still look erased for a while - covered above */
        /* interrupted ERASE (sector recycle): any slot left half-erased -> UNCERTAIN */
        nor_erase(); for (int k = 0; k < 5; k++) { owner_state_t m = model; m.soc = 10 + k; save(&m, false, OWNER_STATE_PAGE); }
        nor[2 * OWNER_STATE_REC + 7] = 0xFF; nor[2 * OWNER_STATE_REC + 9] |= 0x40;            /* partially erased slot 2 */
        CHECK(owner_state_damaged(nor, SECTOR) == 1 && owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_UNCERTAIN);
        /* recycle: the next save with replies OFF erases and writes cleanly; inside a pause (replies on) it does not erase */
        owner_state_t again = model; again.soc = 14;
        owner_job_t j; CHECK(owner_state_plan(nor, &again, true, &j) == OWNER_PLAN_SAME);      /* replies on: no erase */
        CHECK(save(&again, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE);                      /* replies off: erase + write */
        CHECK(owner_state_damaged(nor, SECTOR) == 0 && owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_OFFER && b.soc == 14);
        /* power cut between the erase and the program of a recycle: empty sector -> NONE */
        nor_erase(); CHECK(owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_NONE);
        /* full sector + STOP with replies off: erase then write "none" -> NONE */
        nor_erase();
        for (unsigned k = 0; k < SECTOR / OWNER_STATE_REC; k++) { owner_state_t m = model; m.soc = 1 + (int)(k % 90); save(&m, false, OWNER_STATE_PAGE); }
        CHECK(save(&none, false, OWNER_STATE_PAGE) == OWNER_PLAN_WRITE && owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_NONE);
        /* flash_safe_execute failure (nothing programmed): the old record stays -> OFFER only, never an automatic start */
        nor_erase(); save(&model, false, OWNER_STATE_PAGE);
        CHECK(owner_state_plan(nor, &none, false, &j) == OWNER_PLAN_WRITE);                    /* planned, not executed */
        CHECK(owner_state_boot(nor, SECTOR, &b) == OWNER_BOOT_OFFER);
        printf("R110-1: torn STOP at every byte -> never OFFER (%u torn cases); interrupted erase -> UNCERTAIN; recycle cleans\n", cases);
    }
    /* review 105 R2: when a pending record may be written (boundaries of the quiet gap; never with replies on and a
     * busy bus; a stopped bus writes at once) */
    {   const uint32_t Q = 20000u;
        CHECK(owner_state_when(false, true, false, UINT32_MAX, Q) == OWNER_WHEN_IDLE);   /* nothing pending */
        CHECK(owner_state_when(true, false, false, UINT32_MAX, Q) == OWNER_WHEN_IDLE);   /* 1.5 s settle not reached */
        CHECK(owner_state_when(true, true, false, 0, Q) == OWNER_WHEN_NOW);              /* replies off: write now */
        CHECK(owner_state_when(true, true, true, 0, Q) == OWNER_WHEN_LATER);             /* event just now */
        CHECK(owner_state_when(true, true, true, Q - 1, Q) == OWNER_WHEN_LATER);         /* 1 us short of the gap */
        CHECK(owner_state_when(true, true, true, Q, Q) == OWNER_WHEN_PAUSE);
        CHECK(owner_state_when(true, true, true, UINT32_MAX, Q) == OWNER_WHEN_PAUSE);
        /* inside a pause the save is planned as "running": a full sector waits for replies off instead of erasing */
        static uint8_t full[OWNER_STATE_SECTOR]; memset(full, 0xFF, sizeof full);
        owner_state_t w = {"P12PM-B2", 40, 0}; static owner_job_t job;
        for (unsigned k = 0; k < OWNER_STATE_SECTOR / OWNER_STATE_REC; k++) {
            w.soc = 1 + (int)(k % 100); w.seq = k + 1; owner_state_pack(&w, full + k * OWNER_STATE_REC);
        }
        w.soc = 77;
        CHECK(owner_state_plan(full, &w, true, &job) == OWNER_PLAN_WAIT_ERASE);
        CHECK(owner_state_plan(full, &w, false, &job) == OWNER_PLAN_WRITE && job.erase);
    }
    printf("owner_state: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
