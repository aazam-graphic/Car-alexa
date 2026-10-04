/*
 * game_serpent.h - NEON SERPENT V2 on the 120-LED RGB matrix.
 *
 * Matrix: 12x10 landscape / 10x12 portrait.
 * Gameplay: Y=UP, A=DOWN, X=LEFT, B=RIGHT.
 * D-pad, sticks, LT/RT and LB/RB are ignored during gameplay.
 * START pause/resume; BACK tap menu, BACK hold exit; GUIDE remains E-STOP.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

void serpent_arcade_init(void);
bool serpent_arcade_active(void);
bool serpent_arcade_route(const xbox360_pad_t *pad, uint16_t dig,
                          uint16_t tap, uint32_t now_ms);
void serpent_arcade_tick(uint32_t now_ms);

uint8_t serpent_rumble_mode(void);
void serpent_set_rumble_mode(uint8_t m);
uint32_t serpent_hiscore(void);
uint8_t serpent_arcade_state(void);
bool serpent_suppress_oled(void);
bool serpent_arcade_open(void);

/* TFT companion snapshot. Same car_task context: no heap, no lock required. */
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
    uint8_t  fw, fh;
    uint8_t  winfx, pad_lost;
    uint8_t  frame_rgb[120][3];
    uint32_t mirror_dirty[4];
    /* V2 analytics/gameplay state */
    uint8_t  combo, multiplier, max_combo;
    uint8_t  power, shield, slow, ghost;
    uint16_t power_ms_left;
    uint32_t foods_total, longest_len;
    uint32_t play_time_ms, level_time_ms;
    uint32_t level_score, bonuses;
} serpent_status_t;

/* Legacy Games-Hub ABI kept intentionally; real V2 gameplay is matrix route/tick. */
void serpent_mx_init(void);
void serpent_mx_update(const xbox360_pad_t *pad, uint32_t now_ms);
void serpent_mx_draw(uint32_t now_ms);

void serpent_arcade_get_status(serpent_status_t *out);

#ifdef __cplusplus
}
#endif
