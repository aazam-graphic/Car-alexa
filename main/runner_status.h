#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    RUNNER_OFF,
    RUNNER_MENU,
    RUNNER_COUNTDOWN,
    RUNNER_PLAYING,
    RUNNER_PAUSED,
    RUNNER_DYING,
    RUNNER_LEVEL_CLEAR,
    RUNNER_GAME_OVER
} runner_state_t;

typedef struct {
    runner_state_t state;
    uint32_t score;
    uint8_t level;
    uint8_t lives;
    uint16_t energy;
    int16_t player_x;
    int16_t player_y;
    int16_t camera_x;
    bool grounded;
} runner_status_t;

typedef struct {
    int16_t x_fp;
    int16_t y_fp;
    int16_t vx_fp;
    int16_t vy_fp;
    bool grounded;
    bool alive;
    uint8_t lives;
} runner_player_t;
