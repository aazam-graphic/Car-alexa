/*
 * game_serpent.h - NEON SERPENT ARENA on 120-LED matrix (blueprint Game 1).
 *
 * Entry: BACK hold 1s while parked (STANDBY/HOME...), no TFT game, no estop.
 * While active the arcade OWNS the controller: car.c skips input_handle
 * and forces motors 0. GUIDE is never consumed (car E-STOP always runs).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

void serpent_arcade_init(void);   /* NVS hiscore load, idempotent */
bool serpent_arcade_active(void); /* MENU/COUNTDOWN/PLAYING/PAUSED/GAME_OVER */

/* Call every car_task loop BEFORE input_handle.
 * Returns true = input consumed (skip car controls, motors stay 0). */
bool serpent_arcade_route(const xbox360_pad_t *pad, uint16_t dig,
                          uint16_t tap, uint32_t now_ms);

/* rumble mode 0 OFF 1 SOFT 2 FULL (NVS arcade/rumble, SETTINGS item) */
uint8_t serpent_rumble_mode(void);
void serpent_set_rumble_mode(uint8_t m);

/* hi-score for HOME tile + hub (serp_hi2, full runs only) */
uint32_t serpent_hiscore(void);

/* arcade state for OLED gating / stats (ARC_* as uint8) */
uint8_t serpent_arcade_state(void);
/* true in COUNTDOWN/INTRO/PLAY/DYING/CLEAR: skip periodic OLED redraw */
bool serpent_suppress_oled(void);

/* per-loop game progression (steps/timers/renders). Caller-independent:
   input handling stays in serpent_arcade_route. */
void serpent_arcade_tick(uint32_t now_ms);

/* TFT companion snapshot (same car_task, no lock needed) */
typedef struct {
    uint8_t  state;
    uint32_t score, hi;
    uint8_t  level, level_count;
    const char *level_name;
    uint8_t  food_in_level, food_target;
    uint8_t  len;
    uint16_t step_ms;
    uint8_t  wrap, mines, rumble_mode, new_hi, practice, endless, death_reason;
    uint16_t bonus_ms_left;
    uint8_t  best_level, lives;
    uint8_t  fw, fh;                 /* matrix field (12x10 / 10x12) */
    uint8_t  frame_rgb[120][3];   /* logical index order */
    uint32_t mirror_dirty[4];     /* 120-bit, reader clears after redraw */
} serpent_status_t;
void serpent_arcade_get_status(serpent_status_t *out);

/* Games-Hub registry stubs (TFT game flow needs init/update/draw;
   the real game runs on the matrix via route/tick) */
void serpent_mx_init(void);
void serpent_mx_update(const xbox360_pad_t *pad, uint32_t now_ms);
void serpent_mx_draw(uint32_t now_ms);

/* open from Games Hub / HOME tile. Checks preconditions
   (matrix ready, no estop, pad paired, parked, no TFT game).
   True = arcade MENU entered. */
bool serpent_arcade_open(void);

#ifdef __cplusplus
}
#endif
