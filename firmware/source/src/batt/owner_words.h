/* OWNER phone-test build: the battery words the emulator COMPUTES instead of replaying the recording.
 *   0x0D RelativeStateOfCharge = owner SOC (1..100)
 *   0x0F RemainingCapacity     = ceil(0x10 * SOC / 100) on the profile's OWN full-charge word 0x10, so that the integer
 *                                100 * 0x0F / 0x10 gives back exactly 0x0D (DeepSeek LIVEBATT F7)
 *   0x09 Voltage               = INA226 bus voltage in mV, only inside 2.80..4.60 V
 * Every image is a list of {cmd, lo, hi} triplets for live_publish_soc(SOC_PUBLISH_WORDS, ...).
 * Pure functions, no hardware: main.c feeds live inputs and publishes the result (review 102 F-04 moved them out of
 * main.c so the host tests exercise the firmware code instead of a hand copy). */
#ifndef CHIPLER_OWNER_WORDS_H
#define CHIPLER_OWNER_WORDS_H
#include <stdbool.h>
#include <stdint.h>

#define OWNER_VBUS_MIN_V 2.80f
#define OWNER_VBUS_MAX_V 4.60f

uint16_t owner_remaining(uint16_t fcc, unsigned soc);
/* {0x0D,soc},{0x0F,remaining}: returns 6, or 0 (image untouched) when soc is not 1..100 or fcc is 0. */
unsigned owner_soc_image(uint8_t img[6], int soc, uint16_t fcc);
/* {0x09,mV}: returns 3, or 0 outside OWNER_VBUS_MIN_V..OWNER_VBUS_MAX_V (or NaN). */
unsigned owner_volt_image(uint8_t img[3], float v_bus);
/* Rewrites every 0x0F triplet of a WORDS image from the 0x0D triplet of the same image (0..100), so a soc_apply()
 * image built on the T06 full charge lands on the live target's own 0x10. False when nothing was rewritten. */
bool owner_rescale_words(uint8_t *img, unsigned len, uint16_t fcc);

/* review 105 R1: the words a DATA TEST target must already serve when it starts answering (restore, explicit
 * choice, restart after recovery): 0x0D/0x0F from soc on the target's own 0x10, plus 0x09 when v_ok and in range,
 * in ONE atomic publish. Called between prepare and activate. False (publish nothing usable) = keep the target off. */
typedef struct {
    bool (*word)(void *ctx, uint8_t cmd, uint16_t *v);
    bool (*publish)(void *ctx, uint8_t base, const uint8_t *data, uint8_t len);
    void *ctx;
} owner_bus_t;
bool owner_initial_words(const owner_bus_t *b, int soc, bool v_ok, float v_bus);
#endif
