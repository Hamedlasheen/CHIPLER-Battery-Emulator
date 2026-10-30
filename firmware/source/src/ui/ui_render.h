#ifndef CHIPLER_UI_RENDER_H
#define CHIPLER_UI_RENDER_H
#include "ui_model.h"
/* Render the full screen for model m into the gfx framebuffer (gfx_init must be called). */
void ui_render(const ui_model_t *m);
#endif
