/*
 * serpent_levels.h - 10 level table (MATRIX_SNAKE_BLUEPRINT v1.0, Section 7).
 * Walls as 10x uint16 bitmasks (bit x = block). Flash/rodata, zero RAM.
 * Row 5 is clear in every map (snake start row).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint16_t step_ms;
    uint8_t  wrap;          /* 1 = WRAP, 0 = WALL (edges deadly) */
    uint8_t  food_target;
    uint8_t  mines_start, mines_max;
    uint8_t  patrol;        /* 0/1 moving mine */
    uint8_t  patrol_row;
    uint8_t  bonus_on;
    const uint16_t *block_rows;   /* 10 rows */
} sp_level_t;

static const uint16_t LVL_EMPTY[10]   = {0,0,0,0,0,0,0,0,0,0};
static const uint16_t LVL_PILLARS[10] = {0,0,0x204,0,0,0,0,0x204,0,0};
static const uint16_t LVL_BARS[10]    = {0,0,0x078,0,0,0,0,0x1E0,0,0};
static const uint16_t LVL_CORNERS[10] = {0x606,0x402,0,0,0,0,0,0,0x402,0x606};
static const uint16_t LVL_MAZE[10]    = {0,0x79E,0,0x060,0,0,0x060,0,0x79E,0};
static const uint16_t LVL_DIAMOND[10] = {0,0,0x264,0x108,0,0,0x108,0x264,0,0};

static const sp_level_t SP_LEVELS[10] = {
    { "FIRST BYTE",   320, 1,  6, 0, 0, 0, 0, 0, LVL_EMPTY   },
    { "CLASSIC",      280, 1,  7, 0, 1, 0, 0, 1, LVL_EMPTY   },
    { "SPARK",        240, 1,  8, 1, 2, 0, 0, 1, LVL_EMPTY   },
    { "WALLED",       240, 0,  8, 0, 1, 0, 0, 1, LVL_EMPTY   },
    { "PILLARS",      200, 1,  9, 1, 2, 0, 0, 1, LVL_PILLARS },
    { "BARS",         200, 0,  9, 1, 2, 0, 0, 1, LVL_BARS    },
    { "CORNERS",      160, 1, 10, 2, 3, 0, 0, 1, LVL_CORNERS },
    { "PATROL",       160, 0, 10, 1, 2, 1, 2, 1, LVL_EMPTY   },
    { "MAZE-LITE",    120, 0, 10, 2, 3, 0, 0, 1, LVL_MAZE    },
    { "NOKIA MASTER", 120, 0, 12, 2, 3, 1, 1, 1, LVL_DIAMOND },
};

static inline const sp_level_t *sp_level(uint8_t lv)  /* 1..10 */
{
    if (lv < 1) lv = 1;
    if (lv > 10) lv = 10;
    return &SP_LEVELS[lv - 1];
}

#ifdef __cplusplus
}
#endif
