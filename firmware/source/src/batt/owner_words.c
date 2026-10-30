#include "owner_words.h"
#include "profile.h"

uint16_t owner_remaining(uint16_t fcc, unsigned soc) { return (uint16_t)(((uint32_t)fcc * soc + 99u) / 100u); }

unsigned owner_soc_image(uint8_t img[6], int soc, uint16_t fcc) {
    if (soc < 1 || soc > 100 || !fcc) return 0;
    uint16_t rem = owner_remaining(fcc, (unsigned)soc);
    img[0] = 0x0D; img[1] = (uint8_t)soc; img[2] = 0;
    img[3] = 0x0F; img[4] = (uint8_t)rem; img[5] = (uint8_t)(rem >> 8);
    return 6;
}

unsigned owner_volt_image(uint8_t img[3], float v_bus) {
    if (!(v_bus >= OWNER_VBUS_MIN_V && v_bus <= OWNER_VBUS_MAX_V)) return 0;   /* also false for NaN */
    uint16_t mv = (uint16_t)(v_bus * 1000.0f + 0.5f);
    img[0] = 0x09; img[1] = (uint8_t)mv; img[2] = (uint8_t)(mv >> 8);
    return 3;
}

bool owner_rescale_words(uint8_t *img, unsigned len, uint16_t fcc) {
    if (!img || !fcc || len % 3) return false;
    int soc = -1;
    for (unsigned k = 0; k < len; k += 3) if (img[k] == 0x0D) soc = img[k + 1] | (img[k + 2] << 8);
    if (soc < 0 || soc > 100) return false;
    bool changed = false;
    for (unsigned k = 0; k < len; k += 3)
        if (img[k] == 0x0F) {
            uint16_t r = owner_remaining(fcc, (unsigned)soc);
            img[k + 1] = (uint8_t)r; img[k + 2] = (uint8_t)(r >> 8); changed = true;
        }
    return changed;
}

bool owner_initial_words(const owner_bus_t *b, int soc, bool v_ok, float v_bus) {
    uint16_t fcc;
    uint8_t img[9];
    if (!b || !b->word || !b->publish || !b->word(b->ctx, 0x10, &fcc)) return false;
    unsigned n = owner_soc_image(img, soc, fcc);
    if (!n) return false;
    if (v_ok) n += owner_volt_image(img + n, v_bus);   /* out of range: the recorded 0x09 stays, not a failure */
    return b->publish(b->ctx, SOC_PUBLISH_WORDS, img, (uint8_t)n);
}
