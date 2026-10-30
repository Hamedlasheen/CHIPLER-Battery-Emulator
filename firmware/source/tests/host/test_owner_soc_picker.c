/* OWNER r7: SOC picker on SCR_SOC - any integer 1..100, steps -10/-1/+1/+10, presets 25/50/100, APPLY -> CONFIRM ->
 * ACT_SOC_APPLY with the picked value, and soc_apply() publishes 0x0D/0x0F for that value on a 12-family profile. */
#include "../../src/ui/ui_nav.h"
#include "../../src/batt/soc_ctrl.h"
#include "../../src/batt/smbus_target.h"
#include "../../src/batt/live_prepare.h"
#include <stdio.h>
#include <string.h>
static unsigned checks, fails, masked;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
static uint32_t guard(void *p){(void)p;unsigned o=masked;masked=1;return o;}
static void unguard(void *p,uint32_t v){(void)p;masked=v;}
static st_target_t g_t;
static bool pub(void *ctx,uint8_t base,const uint8_t *d,uint8_t len){(void)ctx;return live_publish_soc(&g_t,base,d,len);}
static bool rd_word(st_target_t *t, uint8_t cmd, uint16_t *v) {
    uint8_t d[2], pec;
    if (!st_on_rx(t, cmd) || !st_begin_read(t)) return false;
    if (!st_on_tx(t, &d[0]) || !st_on_tx(t, &d[1]) || !st_on_tx(t, &pec)) return false;
    st_on_nack(t); if (st_on_stop(t) != ST_OK) return false;
    *v = (uint16_t)(d[0] | (d[1] << 8)); return true;
}
int main(void) {
    /* soc_ctrl accepts any 1..100, rejects 0 and 101 */
    C(soc_valid(1) && soc_valid(37) && soc_valid(100) && !soc_valid(0) && !soc_valid(101));

    ui_nav_t n; nav_init(&n, profile_count());
    nav_ctx_t c; memset(&c, 0, sizeof c); c.out = OUT_OFF; c.soc_current = 50; c.now = 1000;
    int arg = -1; const char *toast = 0;
    n.screen = SCR_MENU; n.menu_cur = MI_SOC;
    C(nav_handle(&n, BTN_EV_OK, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_SOC && n.soc_pick == 50);
    C(nav_select(&n, 3, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 60);    /* +10 */
    C(nav_select(&n, 1, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 59);    /* -1 */
    C(nav_select(&n, 2, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 60);    /* +1 */
    for (int k = 0; k < 6; k++) nav_select(&n, 3, &c, &arg, &toast);
    C(n.soc_pick == 100);                                                      /* clamp high */
    for (int k = 0; k < 12; k++) nav_select(&n, 0, &c, &arg, &toast);
    C(n.soc_pick == 1);                                                        /* clamp low */
    C(nav_select(&n, OWNER_SOC_PRESET0 + 0, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 25);
    C(nav_select(&n, OWNER_SOC_PRESET0 + 2, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 100);
    C(nav_handle(&n, BTN_EV_DOWN, &c, &arg, &toast) == ACT_NONE && n.soc_pick == 99);   /* hardware step */
    nav_select(&n, 0, &c, &arg, &toast); nav_select(&n, 0, &c, &arg, &toast);
    nav_select(&n, 0, &c, &arg, &toast); nav_select(&n, 0, &c, &arg, &toast);
    nav_select(&n, 0, &c, &arg, &toast); nav_select(&n, 0, &c, &arg, &toast);  /* 99 -> 39 */
    nav_select(&n, 1, &c, &arg, &toast); nav_select(&n, 1, &c, &arg, &toast);  /* -> 37 */
    C(n.soc_pick == 37);
    C(nav_select(&n, OWNER_SOC_APPLY, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_CONFIRM && n.confirm_arg == 37);
    c.now += CONFIRM_ARM_DELAY_MS + 10;
    C(nav_select(&n, 1, &c, &arg, &toast) == ACT_SOC_APPLY && arg == 37 && n.screen == SCR_DASH);
    /* same value as current: refused with a toast, no confirm */
    n.screen = SCR_MENU; n.menu_cur = MI_SOC; c.soc_current = 37;
    nav_handle(&n, BTN_EV_OK, &c, &arg, &toast);
    C(n.soc_pick == 37 && nav_select(&n, OWNER_SOC_APPLY, &c, &arg, &toast) == ACT_NONE && toast && n.screen == SCR_SOC);
    /* BACK leaves to the menu */
    C(nav_back(&n, &c, &arg, &toast) == ACT_NONE && n.screen == SCR_MENU);

    /* soc_apply(37) on a 12-family profile publishes 0x0D = 37 */
    const batt_profile_t *p = profile_get(profile_find("P12P-B2"));
    st_init(&g_t, 0x0B); C(st_set_guard(&g_t, guard, unguard, 0)); C(live_prepare(&g_t, p, 50) && st_live_ready(&g_t));
    soc_ctrl_t s; soc_init(&s);
    C(soc_apply(&s, p, 37, pub, 0) == SOC_RES_APPLIED_PHONE_DATA && s.applied == 37);
    uint16_t v = 0; C(rd_word(&g_t, 0x0D, &v) && v == 37);
    C(soc_apply(&s, p, 0, pub, 0) == SOC_RES_REJECTED && s.applied == 37);
    printf("owner_soc_picker: %u checks, %u failures: %s\n", checks, fails, fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
