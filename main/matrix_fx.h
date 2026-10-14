/*
 * matrix_fx.h - particle engine for the 23x13 / 299 LED matrix shows.
 *
 * Why this exists: a pure per-pixel procedural ring ("fireworks") looks
 * sharp because each pixel snaps on/off every frame. Real fireworks are
 * *moving emitters*, so this module simulates them as real particles with
 * sub-pixel positions and renders them with bilinear splats + additive
 * blending. Sub-pixel = motion is continuous instead of 1-pixel jumps.
 *
 * Rules:
 *  - draws only through logical matrix_add_xy() (never a raw LED index)
 *  - fixed pool, zero malloc, deterministic (seeded from burst slot)
 *  - integer/float math bounded, runs at the show's fixed 30 FPS step
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "led_matrix.h"

/* ---- shared colour helpers (matrix_show.c uses these too) ---- */
rgb_t hsv_rgb(uint16_t h, uint8_t sat, uint8_t val);
void  hsv_put(int x, int y, uint16_t h, uint8_t sat, uint8_t val);

/* effect ids handled by this module (others stay in matrix_show.c) */
#define FX_FIREWORKS 3

/* fixed simulation step, must match matrix_show.c SHOW_FRAME_MS */
#define FX_STEP_MS 30u

/* reset all particles + timeline (show open / effect switch) */
void matrix_fx_reset(void);

/* advance the simulation by exactly one FX_STEP_MS step */
void matrix_fx_step(void);

/* additive render pass: called after the background pixels are drawn */
void matrix_fx_render(int w, int h);

/* night-sky background for one pixel (drawn before the particle pass) */
void matrix_fx_background(int x, int y, int w, int h, uint32_t t);

/* true on the step where a burst ignites (sound hook) */
bool matrix_fx_burst_fired(void);
