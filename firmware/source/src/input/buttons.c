#include "buttons.h"

void buttons_init(buttons_t *b, uint32_t now_ms) {
    for (int i = 0; i < BTN_COUNT; i++) {
        btn_ch_t *c = &b->ch[i];
        c->raw = c->stable = false;
        c->raw_since = now_ms; c->down_at = 0; c->next_repeat = 0;
        c->consumed = false; c->repeating = false;
    }
    b->chord_lock = false;
}

const char *btn_event_name(btn_event_t e) {
    static const char *n[] = {"NONE", "UP", "DOWN", "OK", "OK_LONG"};
    return (unsigned)e < 5 ? n[e] : "?";
}

btn_event_t buttons_update(buttons_t *b, uint32_t now, bool up, bool down, bool ok) {
    bool in[BTN_COUNT] = {up, down, ok};
    bool pressed_edge[BTN_COUNT] = {0}, released_edge[BTN_COUNT] = {0};
    uint32_t held_at_release[BTN_COUNT] = {0};

    for (int i = 0; i < BTN_COUNT; i++) {
        btn_ch_t *c = &b->ch[i];
        if (in[i] != c->raw) { c->raw = in[i]; c->raw_since = now; }
        if (c->raw != c->stable && (uint32_t)(now - c->raw_since) >= BTN_DEBOUNCE_MS) {
            c->stable = c->raw;
            if (c->stable) {
                c->down_at = c->raw_since; c->consumed = false; c->repeating = false;
                pressed_edge[i] = true;
            } else {
                released_edge[i] = true;
                held_at_release[i] = c->raw_since - c->down_at;
            }
        }
    }

    int n_down = 0;
    for (int i = 0; i < BTN_COUNT; i++) if (b->ch[i].stable) n_down++;
    if (n_down >= 2) b->chord_lock = true;
    if (b->chord_lock) {
        /* cancel every press involved in a chord; nothing fires until all are released */
        for (int i = 0; i < BTN_COUNT; i++) if (b->ch[i].stable || released_edge[i]) b->ch[i].consumed = true;
        if (n_down == 0) {
            b->chord_lock = false;
            for (int i = 0; i < BTN_COUNT; i++) b->ch[i].consumed = false;
        }
        return BTN_EV_NONE;
    }
    (void)pressed_edge;

    /* OK long press (fires once while held) */
    btn_ch_t *okc = &b->ch[BTN_OK];
    if (okc->stable && !okc->consumed && (uint32_t)(now - okc->down_at) >= BTN_OK_LONG_MS) {
        okc->consumed = true;
        return BTN_EV_OK_LONG;
    }
    /* UP / DOWN auto-repeat while held */
    for (int i = BTN_UP; i <= BTN_DOWN; i++) {
        btn_ch_t *c = &b->ch[i];
        if (!c->stable || c->consumed) continue;
        uint32_t held = now - c->down_at;
        if (!c->repeating && held >= BTN_REPEAT_START_MS) {
            c->repeating = true; c->next_repeat = now + BTN_REPEAT_MS;
            return i == BTN_UP ? BTN_EV_UP : BTN_EV_DOWN;
        }
        if (c->repeating && (int32_t)(now - c->next_repeat) >= 0) {
            c->next_repeat = now + BTN_REPEAT_MS;
            return i == BTN_UP ? BTN_EV_UP : BTN_EV_DOWN;
        }
    }
    /* short presses fire on release */
    for (int i = 0; i < BTN_COUNT; i++) {
        btn_ch_t *c = &b->ch[i];
        if (!released_edge[i]) continue;
        bool fire = !c->consumed && !c->repeating && held_at_release[i] >= BTN_SHORT_MIN_MS;
        if (i == BTN_OK) fire = fire && held_at_release[i] < BTN_OK_LONG_MS;
        c->consumed = false; c->repeating = false;
        if (fire) return i == BTN_UP ? BTN_EV_UP : (i == BTN_DOWN ? BTN_EV_DOWN : BTN_EV_OK);
    }
    return BTN_EV_NONE;
}
