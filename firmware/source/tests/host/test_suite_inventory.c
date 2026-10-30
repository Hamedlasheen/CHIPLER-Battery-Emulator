/* Review 110 R110-3: the owner contract suites are registered exactly when the code under test is compiled as the
 * owner build. OWNER_SUITES_REGISTERED comes from tests/host/CMakeLists.txt; CHIPLER_OWNER_PHONE_TEST from the same
 * flags the suites are built with. Any spelling of the flag the preprocessor accepts (=1, =0x1, ...) must agree. */
#include <stdio.h>
#ifndef CHIPLER_OWNER_PHONE_TEST
#define CHIPLER_OWNER_PHONE_TEST 0
#endif
int main(void) {
    int owner = CHIPLER_OWNER_PHONE_TEST ? 1 : 0;
    int ok = owner ? OWNER_SUITES_REGISTERED == 3 : OWNER_SUITES_REGISTERED == 0;
    printf("suite inventory: compiled owner=%d, owner suites registered=%d: %s\n", owner, OWNER_SUITES_REGISTERED, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
