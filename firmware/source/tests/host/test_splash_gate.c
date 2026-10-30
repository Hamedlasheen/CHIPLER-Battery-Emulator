/* 063 R1: splash acknowledgement logic: failed push / no ack, successful startup push, fallback timeout. */
#include "../../src/ui/splash_gate.h"
#include <stdio.h>
static unsigned checks, fails;
#define C(x) do{checks++;if(!(x)){fails++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
int main(void) {
    splash_gate_t g;
    /* failed push, empty push, non-startup screen: never acknowledge; leave only via the 8 s fallback */
    splash_gate_init(&g);
    splash_gate_on_push(&g, true, 240, false); splash_gate_on_push(&g, true, 0, true); splash_gate_on_push(&g, false, 240, true);
    C(!atomic_load(&g.on_panel));
    for (uint32_t t = 100; t <= 8000; t += 100) C(!splash_gate_leave(&g, t));      /* no ack: splash stays (timer not started) */
    C(!splash_gate_leave(&g, 8001) && g.t0 == 8001);                                /* fallback arms the timer */
    C(!splash_gate_leave(&g, 8001 + 2499) && splash_gate_leave(&g, 8001 + 2500));
    /* successful startup push: timer starts at the next core-0 pass, splash held 2.5 s after it */
    splash_gate_init(&g);
    C(!splash_gate_leave(&g, 500));
    splash_gate_on_push(&g, true, 240, true);
    C(atomic_load(&g.on_panel));
    C(!splash_gate_leave(&g, 700) && g.t0 == 700);
    C(!splash_gate_leave(&g, 3199) && splash_gate_leave(&g, 3200));
    /* now == 0 can never leave t0 == 0 (would restart the gate) */
    splash_gate_init(&g); splash_gate_on_push(&g, true, 1, true); C(!splash_gate_leave(&g, 0) && g.t0 == 1);
    /* wrap-around safe */
    splash_gate_init(&g); splash_gate_on_push(&g, true, 1, true); C(!splash_gate_leave(&g, 0xFFFFFF00u) && splash_gate_leave(&g, 0xFFFFFF00u + 2500u));
    printf("splash_gate: %u checks, %u failures\n", checks, fails);
    return fails != 0;
}
