/*
 * serpent_levels.h - NEON SERPENT V2: 20 handcrafted matrix levels + Endless.
 * 120 LEDs: landscape 12x10, portrait 10x12 (transpose handled by game).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint16_t step_ms;
    uint8_t  wrap;
    uint8_t  food_target;
    uint8_t  mines_start, mines_max;
    uint8_t  patrol;          /* moving hazard enabled */
    uint8_t  patrol_row;
    uint8_t  bonus_on;        /* power-up system enabled */
    uint8_t  hazard_style;    /* 0 static, 1 patrol, 2 hunter, 3 dual */
    uint8_t  special_rate;   /* % chance special food */
    const uint16_t *block_rows;
} sp_level_t;

static const uint16_t LVL_EMPTY[10] =
    {0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000};
static const uint16_t LVL_PILLARS[10] =
    {0x000,0x000,0x204,0x000,0x000,0x000,0x000,0x204,0x000,0x000};
static const uint16_t LVL_BARS[10] =
    {0x000,0x000,0x078,0x000,0x000,0x000,0x000,0x1E0,0x000,0x000};
static const uint16_t LVL_CORNERS[10] =
    {0x606,0x402,0x000,0x000,0x000,0x000,0x000,0x000,0x402,0x606};
static const uint16_t LVL_MAZE[10] =
    {0x000,0x79E,0x000,0x060,0x000,0x000,0x060,0x000,0x79E,0x000};
static const uint16_t LVL_DIAMOND[10] =
    {0x000,0x000,0x264,0x108,0x000,0x000,0x108,0x264,0x000,0x000};
static const uint16_t LVL_CROSS[10] =
    {0x000,0x000,0x018,0x018,0x000,0x000,0x180,0x180,0x000,0x000};
static const uint16_t LVL_GATES[10] =
    {0x000,0x3C0,0x000,0x030,0x000,0x000,0x030,0x000,0x03C,0x000};
static const uint16_t LVL_SPINE[10] =
    {0x000,0x0C0,0x0C0,0x000,0x660,0x000,0x066,0x006,0x006,0x000};
static const uint16_t LVL_BOXES[10] =
    {0x000,0x7FE,0x402,0x402,0x000,0x000,0x402,0x402,0x7FE,0x000};
static const uint16_t LVL_ZIGZAG[10] =
    {0x000,0x0E0,0x000,0x01C,0x000,0x000,0x1C0,0x000,0x038,0x000};
static const uint16_t LVL_TWIN[10] =
    {0x000,0x300,0x000,0x00C,0x000,0x000,0x00C,0x000,0x300,0x000};
static const uint16_t LVL_WINDOW[10] =
    {0x000,0x7E0,0x420,0x420,0x000,0x000,0x420,0x420,0x7E0,0x000};
static const uint16_t LVL_CHANNEL[10] =
    {0x000,0x7C0,0x040,0x040,0x000,0x000,0x010,0x010,0x07C,0x000};
static const uint16_t LVL_RINGS[10] =
    {0x000,0x3C0,0x240,0x240,0x000,0x000,0x042,0x042,0x03C,0x000};
static const uint16_t LVL_FORK[10] =
    {0x000,0x110,0x110,0x7FE,0x000,0x000,0x7FE,0x011,0x011,0x000};
static const uint16_t LVL_MIRROR[10] =
    {0x000,0x181,0x000,0x066,0x000,0x000,0x660,0x000,0x181,0x000};
static const uint16_t LVL_FINAL[10] =
    {0x000,0x3C0,0x042,0x1CE,0x000,0x000,0x1CE,0x042,0x03C,0x000};

static const sp_level_t SP_LEVELS[20] = {
    {"FIRST BYTE",      300, 1,  8, 0,1, 0,0, 1,0,  5, LVL_EMPTY},
    {"CLASSIC",         280, 1,  9, 0,1, 0,0, 1,0,  7, LVL_EMPTY},
    {"PILLAR RUN",      255, 1, 10, 1,2, 0,0, 1,0,  8, LVL_PILLARS},
    {"WALLED",          245, 0, 10, 0,1, 0,0, 1,0, 10, LVL_EMPTY},
    {"BARS",            225, 0, 11, 1,2, 0,0, 1,0, 10, LVL_BARS},
    {"DIAMOND",         215, 1, 11, 1,2, 0,0, 1,0, 12, LVL_DIAMOND},
    {"CORNERS",         205, 1, 12, 2,3, 0,0, 1,0, 12, LVL_CORNERS},
    {"GATES",           195, 0, 12, 2,3, 1,5, 1,1, 14, LVL_GATES},
    {"MAZE-LITE",       185, 0, 13, 2,3, 0,0, 1,0, 14, LVL_MAZE},
    {"SPINE",           175, 0, 13, 2,3, 1,2, 1,1, 15, LVL_SPINE},
    {"BOX RUN",         165, 1, 14, 2,3, 1,2, 1,1, 15, LVL_BOXES},
    {"ZIGZAG",          155, 0, 14, 2,4, 1,4, 1,2, 16, LVL_ZIGZAG},
    {"TWIN LANES",      145, 1, 15, 2,4, 1,5, 1,2, 16, LVL_TWIN},
    {"WINDOW",          135, 0, 15, 3,4, 1,4, 1,2, 17, LVL_WINDOW},
    {"CHANNEL",         125, 0, 16, 3,4, 1,3, 1,2, 18, LVL_CHANNEL},
    {"RINGS",           118, 1, 16, 3,4, 1,4, 1,2, 18, LVL_RINGS},
    {"FORK",            110, 0, 17, 3,5, 1,5, 1,3, 19, LVL_FORK},
    {"MIRROR MAZE",     104, 0, 17, 3,5, 1,2, 1,3, 19, LVL_MIRROR},
    {"SURVIVAL",         98, 1, 18, 4,5, 1,4, 1,3, 20, LVL_CROSS},
    {"FINAL ARENA",      94, 0, 20, 4,6, 1,5, 1,3, 22, LVL_FINAL},
};

static inline const sp_level_t *sp_level(uint8_t lv)
{
    if (lv < 1) lv = 1;
    if (lv > 20) lv = 20;
    return &SP_LEVELS[lv - 1];
}

#ifdef __cplusplus
}
#endif
