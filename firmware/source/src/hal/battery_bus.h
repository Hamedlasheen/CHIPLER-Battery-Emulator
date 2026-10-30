/* Battery-side I2C target on I2C1 (via level translator), backed by target_engine. */
#ifndef CHIPLER_BATTERY_BUS_H
#define CHIPLER_BATTERY_BUS_H
#include <stdbool.h>
#include "../batt/target_engine.h"
#include "../batt/smbus_target.h"

void battery_bus_init(void);                         /* translator disabled, target off */
bool battery_bus_start(const batt_profile_t *p);     /* enable target at profile address */
bool battery_bus_prepare(const batt_profile_t *p, int soc);
bool battery_bus_activate(void);                    /* requires successful preparation */
/* review 105 R1: prepare -> initial(ctx) while the target cannot be addressed -> activate. initial may be NULL;
 * when it returns false the target stays off. Only OK leaves the target running. */
typedef enum { BB_START_OK = 0, BB_START_PREPARE, BB_START_WORDS, BB_START_ACTIVATE } bb_start_t;
bb_start_t battery_bus_start_seq(const batt_profile_t *p, int soc, bool (*initial)(void *), void *ctx);
/* review 105 R2: run fn with the live target switched off (address NACKed), then re-arm the same map.
 * false: nothing was paused (not live/running), or the re-arm failed (the target is then off). */
bool battery_bus_pause(void (*fn)(void *), void *ctx);
void battery_bus_stop(void);                         /* disable target + translator */
bool battery_bus_running(void);
typedef struct {uint32_t reads,short_reads,writes,errors,irq,abort_source;uint8_t command;st_result_t fault;} battery_bus_diag_t;
bool battery_bus_diagnostic(battery_bus_diag_t *);
bool battery_bus_fault(void);
void battery_bus_reset_fault(void);
bool battery_bus_publish(uint8_t cmd,const uint8_t *,uint8_t len);
bool battery_bus_word(uint8_t cmd,uint16_t *v);   /* current WORD register value of the live target */
bool battery_bus_history(uint32_t h[8],uint8_t *site); /* live port IRQ history + live_wire fail site (diagnostics) */
uint32_t battery_bus_idle_us(uint32_t now_us);         /* time since the last bus event; UINT32_MAX when not running */
void battery_bus_service(uint32_t now_us);
bool battery_bus_pop_write(st_write_t *);
/* DEEPSEEK AUTH-GATE 082: evidence-gated status; authentication is never claimed. */
typedef struct {int auth_state;const char *auth_str;uint32_t refused_reads,refused_writes,
    dynamic_refusals,servable,selector_pages,unknown_pages;uint8_t last_refused_cmd,
    last_selector_target;} battery_bus_auth_t;
bool battery_bus_auth_status(battery_bus_auth_t *);
/* "AUTH=UNVERIFIED(no_authentication_exchange_identified)" unless real evidence exists. */
const char *battery_bus_auth_state_str(void);
void battery_bus_clear_counters(void);
target_engine_t *battery_bus_engine(void);
#if CHIPLER_AUTO_LEARN
/* AUTO: listen = the prepared live target ACKs its address, records requests and answers every read 0xFF.
 * May be called before activate (no guard yet) or while live (guarded, effective at the next read). */
void battery_bus_set_listen(bool on);
bool battery_bus_listening(void);
bool battery_bus_pop_obs(uint32_t *packed);
uint32_t battery_bus_obs_dropped(void);
#endif
#endif
