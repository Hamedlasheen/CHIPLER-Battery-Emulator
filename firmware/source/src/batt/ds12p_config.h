/* DEEPSEEK AUTH-GATE 082 - compile-time switch (default OFF).
 * OFF: firmware behaviour is exactly as before; no 12Pro reference profile, no hooks.
 * ON : adds the "12 Pro CAP REF" profile, serves the recorded POWER reference rows and
 *      enables the selector-page / evidence-gate wire hooks.
 * Scope: the CHARGER evidence tables are shipped as data but are NOT wired into this
 * profile (a separate decision); 12-family sharing is untouched.
 * Authentication is NEVER claimed: no authentication exchange has been identified, so the
 * gate always reports AUTH=UNVERIFIED. Enable only after review + owner decision. */
#ifndef CHIPLER_DS12P_CONFIG_H
#define CHIPLER_DS12P_CONFIG_H
#ifndef CHIPLER_12P_REFERENCE
#define CHIPLER_12P_REFERENCE 0
#endif
#endif
