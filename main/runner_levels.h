#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    RUNNER_EMPTY = 0,
    RUNNER_PLATFORM = 1,
    RUNNER_SPIKE = 2,
    RUNNER_ENERGY = 3,
    RUNNER_ENEMY = 4,
    RUNNER_EXIT = 5,
    RUNNER_MOVING_BLOCK = 6,
} runner_tile_t;

typedef struct {
    uint8_t type;
    int16_t world_x;
    int8_t y;
    int8_t width;
    int8_t height;
    bool active;
} runner_object_t;

typedef struct {
    const char *name;
    int16_t width;
    int16_t spawn_x, spawn_y;
    int16_t exit_x, exit_y;
    const runner_object_t *objects;
    uint8_t object_count;
    uint8_t difficulty;
    uint8_t palette;
} runner_level_t;

/* ---- level 1: INTRO RUINS ---- */
static const runner_object_t L1_OBJS[] = {
    {RUNNER_PLATFORM, 0, 10, 18, 1, true},
    {RUNNER_PLATFORM, 20, 10, 15, 1, true},
    {RUNNER_ENERGY,   7,  6,  1, 1, true},
    {RUNNER_ENERGY,   12, 6,  1, 1, true},
    {RUNNER_ENERGY,   22, 6,  1, 1, true},
    {RUNNER_EXIT,     32, 6,  2, 4, true},
};
/* ---- level 2: GAP CIRCUIT ---- */
static const runner_object_t L2_OBJS[] = {
    {RUNNER_PLATFORM, 0,  10, 9,  1, true},
    {RUNNER_PLATFORM, 12, 10, 8,  1, true},
    {RUNNER_PLATFORM, 23, 10, 7,  1, true},
    {RUNNER_PLATFORM, 33, 10, 10, 1, true},
    {RUNNER_PLATFORM, 46, 10, 14, 1, true},
    {RUNNER_ENERGY,   4,  6,  1, 1, true},
    {RUNNER_ENERGY,   14, 6,  1, 1, true},
    {RUNNER_ENERGY,   25, 6,  1, 1, true},
    {RUNNER_ENERGY,   38, 6,  1, 1, true},
    {RUNNER_EXIT,     57, 6,  2, 4, true},
};
/* ---- level 3: DRONE PASS ---- */
static const runner_object_t L3_OBJS[] = {
    {RUNNER_PLATFORM, 0, 10, 60, 1, true},
    {RUNNER_ENEMY,    14, 8,  2, 2, true},
    {RUNNER_ENEMY,    29, 8,  2, 2, true},
    {RUNNER_ENEMY,    44, 8,  2, 2, true},
    {RUNNER_ENERGY,   10, 6,  1, 1, true},
    {RUNNER_ENERGY,   25, 6,  1, 1, true},
    {RUNNER_ENERGY,   40, 6,  1, 1, true},
    {RUNNER_ENERGY,   55, 6,  1, 1, true},
    {RUNNER_EXIT,     56, 6,  2, 4, true},
};
/* ---- level 4: SPIKE TUNNEL ---- */
static const runner_object_t L4_OBJS[] = {
    {RUNNER_PLATFORM, 0, 10, 60, 1, true},
    {RUNNER_PLATFORM, 0,  4, 60, 1, true},          /* tunnel ceiling */
    {RUNNER_SPIKE,    9,  9,  1, 1, true},
    {RUNNER_SPIKE,    10, 9,  1, 1, true},
    {RUNNER_SPIKE,    24, 9,  1, 1, true},
    {RUNNER_SPIKE,    25, 9,  1, 1, true},
    {RUNNER_SPIKE,    39, 9,  1, 1, true},
    {RUNNER_SPIKE,    40, 9,  1, 1, true},
    {RUNNER_ENERGY,    5, 6,  1, 1, true},
    {RUNNER_ENERGY,   18, 6,  1, 1, true},
    {RUNNER_ENERGY,   33, 6,  1, 1, true},
    {RUNNER_EXIT,     56, 6,  2, 4, true},
};
/* ---- level 5: CORE ESCAPE ---- */
static const runner_object_t L5_OBJS[] = {
    {RUNNER_PLATFORM,  0, 10, 10, 1, true},
    {RUNNER_PLATFORM, 13, 10, 10, 1, true},
    {RUNNER_PLATFORM, 26, 10, 10, 1, true},
    {RUNNER_PLATFORM, 39, 10, 10, 1, true},
    {RUNNER_PLATFORM, 52, 10, 18, 1, true},
    {RUNNER_ENEMY,    16,  8,  2, 2, true},
    {RUNNER_ENEMY,    42,  8,  2, 2, true},
    {RUNNER_SPIKE,    30,  9,  1, 1, true},
    {RUNNER_SPIKE,    31,  9,  1, 1, true},
    {RUNNER_ENERGY,    5,  6,  1, 1, true},
    {RUNNER_ENERGY,   21,  6,  1, 1, true},
    {RUNNER_ENERGY,   47,  6,  1, 1, true},
    {RUNNER_EXIT,     66,  6,  2, 4, true},
};

static const runner_level_t RUNNER_LEVELS[] = {
    {"INTRO RUINS", 35, 1, 6, 32, 6, L1_OBJS, (uint8_t)(sizeof(L1_OBJS)/sizeof(L1_OBJS[0])), 1, 0},
    {"GAP CIRCUIT", 60, 1, 6, 57, 6, L2_OBJS, (uint8_t)(sizeof(L2_OBJS)/sizeof(L2_OBJS[0])), 2, 1},
    {"DRONE PASS",  60, 1, 6, 56, 6, L3_OBJS, (uint8_t)(sizeof(L3_OBJS)/sizeof(L3_OBJS[0])), 3, 2},
    {"SPIKE TUNNEL",60, 1, 6, 56, 6, L4_OBJS, (uint8_t)(sizeof(L4_OBJS)/sizeof(L4_OBJS[0])), 4, 3},
    {"CORE ESCAPE", 70, 1, 6, 66, 6, L5_OBJS, (uint8_t)(sizeof(L5_OBJS)/sizeof(L5_OBJS[0])), 5, 4},
};

#define RUNNER_LEVEL_COUNT 5
