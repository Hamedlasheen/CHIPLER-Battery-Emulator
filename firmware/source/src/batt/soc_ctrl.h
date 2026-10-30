/* Emulated battery SOC presets (CR-003-r1): exactly 25 / 50 / 100 %.
 *
 * requested : last value the user CONFIRMED (shown as "SIM SOC").
 * applied   : value actually published to the protocol backend, or -1 if none.
 * The two differ when the active profile cannot apply SOC (all real-phone profiles today):
 * the UI then shows "NOT SENT" and never claims the phone was updated.
 *
 * Changing SOC never touches the safety state machine, the eFuse output, PSU voltage or
 * current limits, and never qualifies a profile.
 * Initial value after reset = 50 % (RAM only, no flash persistence). Profile change resets
 * requested to 50 % and re-attempts application on the new profile.
 */
#ifndef CHIPLER_SOC_CTRL_H
#define CHIPLER_SOC_CTRL_H
#include <stdbool.h>
#include <stdint.h>
#include "profile.h"

#define SOC_DEFAULT 50
#define SOC_N_PRESETS 3
extern const int k_soc_presets[SOC_N_PRESETS];   /* {25, 50, 100} */

typedef enum {
    SOC_RES_NONE = 0,
    SOC_RES_APPLIED_SYNTHETIC,  /* bench profile: synthetic image published (NOT a phone) */
    SOC_RES_APPLIED_VERIFIED,   /* VERIFIED phone profile with capture-based builder (none yet) */
    SOC_RES_APPLIED_PHONE_DATA, /* phone DATA TEST: response registers 0x0D/0x0F updated; the PHONE has NOT confirmed anything */
    SOC_RES_RECORDED_RETAINED,  /* phone DATA TEST selected: recorded T06 values (SOC 51) still served, nothing applied */
    SOC_RES_NOT_SUPPORTED,      /* profile has no SOC capability -> phone NOT updated */
    SOC_RES_BUSY_RETRY,         /* publish deferred; applied value unchanged, will retry */
    SOC_RES_REJECTED            /* not one of 25/50/100 -> nothing changed */
} soc_result_t;

/* publish callback: returns false if the backend could not take the snapshot now */
typedef bool (*soc_publish_fn)(void *ctx, uint8_t base, const uint8_t *data, uint8_t len);

typedef struct {
    int requested;
    int applied;
    soc_result_t last;
    bool retry_pending;
} soc_ctrl_t;

bool soc_valid(int pct);
void soc_init(soc_ctrl_t *c);
/* User confirmed a new preset. */
soc_result_t soc_apply(soc_ctrl_t *c, const batt_profile_t *p, int pct, soc_publish_fn pub, void *ctx);
/* Called each control cycle: retries a deferred publish. */
soc_result_t soc_service(soc_ctrl_t *c, const batt_profile_t *p, soc_publish_fn pub, void *ctx);
/* Profile changed: reset to default and try to apply on the new profile. */
soc_result_t soc_on_profile_change(soc_ctrl_t *c, const batt_profile_t *p, soc_publish_fn pub, void *ctx);
const char *soc_result_str(soc_result_t r);
#endif
