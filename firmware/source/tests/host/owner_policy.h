/* Build-policy expectations shared by the host tests (review 102).
 *
 * The OWNER phone-test build (-DCHIPLER_OWNER_PHONE_TEST=1) deliberately differs from the default build in a few
 * documented places (docs/ENGINEERING.md "Build configurations"). A test that asserts a default-build contract uses
 * these macros so the SAME assertion also pins the owner contract, instead of being skipped in one configuration:
 *   WR_ERR(x)   result of a rejected data write: default x (port isolates), owner ST_OK (accounted, dropped)
 *   RD_PROBE(x) count-only read of a non-A8 block: default x, owner ST_SHORT_A8 (iPhone 12 boot probe)
 *   OWNER       1 in the owner build
 */
#ifndef CHIPLER_TEST_OWNER_POLICY_H
#define CHIPLER_TEST_OWNER_POLICY_H
#ifndef CHIPLER_OWNER_PHONE_TEST
#define CHIPLER_OWNER_PHONE_TEST 0
#endif
#ifndef CHIPLER_12P_REFERENCE
#define CHIPLER_12P_REFERENCE 0
#endif
#define OWNER CHIPLER_OWNER_PHONE_TEST
#define WR_ERR(x) (OWNER ? ST_OK : (x))
#define RD_PROBE(x) (OWNER ? ST_SHORT_A8 : (x))
#endif
