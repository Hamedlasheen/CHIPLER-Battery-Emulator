/* AG-INA: feeds raw INA226 counts INVERTED from the old prototype's real serial logs into the
 * production conversion path (ina226_init + ina226_read) and compares with an independent
 * datasheet computation. Intended repo location: SOURCE/tests/host/test_ina226_reallog.c
 * (delivered under AG-INA/new_tests/ because edits outside AG-INA are not allowed).
 * NOTE: old logs print no raw words; counts are inverted (see tools_agent/recompute_ina226.py,
 * which also verifies every VEC(...) below against its log line). Assumes CAL=0800h (2048) so
 * Current reg == Shunt reg, exactly as in the old DRV log header. Nothing here is NEW-board data. */
#include <stdio.h>
#include <stdlib.h>
static unsigned checks, failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
#include "test_ina226.c"

typedef struct { int16_t sh; uint16_t bus; int16_t cur; const char *file; int line; const char *vbus_txt; const char *i_txt; const char *p_txt; } vec_t;
#define VEC(sh,bus,cur,file,line,vb,it,pt) {sh,bus,cur,file,line,vb,it,pt}
#define P1 "SERIAL_LOG_2026-10-17_05-15-49.txt"
#define P2 "SERIAL_LOG_2026-10-17_05-19-38.txt"
#define D1 "SERIAL_LOG_2026-10-18_00-34-14.txt"
/* PMID rows: I text is A with 4 decimals (Vshunt/0.010); DRV rows: I text is mA with 2 decimals. */
static const vec_t V[] = {
    VEC( 492, 3174,  492, P1,  30, "3.967", "+0.1230", NULL),
    VEC( 498, 3177,  498, P1, 136, "3.971", "+0.1245", NULL),
    VEC( 497, 3156,  497, P1, 272, "3.945", "+0.1243", NULL),
    VEC(  -1,    0,   -1, P1, 268, "0.000", "-0.0003", NULL),
    VEC(  -2,    0,   -2, P1, 328, "0.000", "-0.0005", NULL),
    VEC( 367, 2357,  367, P2,  30, "2.946", "+0.0918", NULL),
    VEC( 370, 2360,  370, P2, 104, "2.950", "+0.0925", NULL),
    VEC(  -1, 3111,   -1, D1,   5, "3.889", "-0.25", "-0.001"),
    VEC(   0, 3104,    0, D1,   6, "3.880", "+0.00", "+0.000"),
    VEC(  -2, 3103,   -2, D1,  34, "3.879", "-0.50", "-0.002"),
    VEC(   1, 3107,    1, D1,  42, "3.884", "+0.25", "+0.001"),
};

int main(void) {
    for (unsigned k = 0; k < sizeof V / sizeof V[0]; k++) {
        const vec_t *t = &V[k];
        m226_t m; ina226_dev_t d; i226_sample_t s;
        ina226_bus_t bus = {m226_wr, m226_rd, m226_delay, &m, m226_now};
        m226_defaults(&m);
        CHECK(ina226_init(&d, &bus, &CFG10) == I226_OK);
        CHECK(d.cal == 0x0800 && d.alert_limit == 0x0960);             /* same as real DRV header */
        /* independent datasheet math (double) */
        double lsb = 0.00512 / (2048.0 * 0.010);                       /* Eq.1 solved: 250 uA */
        double v = t->bus * 1.25e-3, i = t->cur * lsb;
        int pw = (int)((double)t->cur * t->bus / 20000.0);             /* Eq.4, truncating */
        m226_set_sample(&m, t->sh, t->bus, t->cur, (uint16_t)(pw < 0 ? -pw : pw), true);
        m.now = 10;
        CHECK(ina226_read(&d, &s, 10) && s.valid && s.new_conversion);
        CHECK(s.raw_shunt == t->sh && s.raw_bus == t->bus && s.raw_current == t->cur);
        CHECK(fabs(s.v_bus - v) <= 1e-6 * (fabs(v) + 1));
        CHECK(fabs(s.i - i) <= 1e-6);
        CHECK(fabs(s.p_signed - v * i) <= 1e-5);
        CHECK(fabs((double)i226_shunt_v(t->sh) - t->sh * 2.5e-6) <= 1e-6 * fabs(t->sh * 2.5e-6) + 1e-15);
        CHECK(s.uncalibrated);
        /* reproduce the text the OLD firmware printed */
        char b[32];
        snprintf(b, sizeof b, "%.3f", s.v_bus); CHECK(strcmp(b, t->vbus_txt) == 0);
        if (t->p_txt) {                                                 /* DRV family */
            snprintf(b, sizeof b, "%+.2f", s.i * 1e3); CHECK(strcmp(b, t->i_txt) == 0);
            snprintf(b, sizeof b, "%+.3f", s.p_signed); CHECK(strcmp(b, t->p_txt) == 0);
        } else {                                                        /* PMID family: I = Vshunt/0.010 */
            /* PMID printed 4 decimals; exact print ties (e.g. 0.12425) round differently in float/double, so compare numerically within half a print step */

            CHECK(fabs(atof(t->i_txt) - (double)i226_shunt_v(t->sh) / 0.010) <= 0.5e-4 + 1e-7);
        }
    }
    printf("Real-log INA226 conversion: %u checks, %u failures (%u vectors)\n", checks, failures, (unsigned)(sizeof V / sizeof V[0]));
    return failures ? 1 : 0;
}
