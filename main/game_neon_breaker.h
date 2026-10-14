#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NEON BREAKER: CORE WAR — 23x13 / 299 LED matrix arcade game. */
void neon_breaker_init(void);
bool neon_breaker_active(void);
bool neon_breaker_open(void);
bool neon_breaker_route(const xbox360_pad_t *pad, uint16_t dig,
                        uint16_t tap, uint32_t now_ms);
void neon_breaker_tick(uint32_t now_ms);
uint8_t neon_breaker_state(void);
bool neon_breaker_suppress_oled(void);

/* Games Hub registry hooks. */
void neon_breaker_mx_init(void);
void neon_breaker_mx_update(const xbox360_pad_t *pad, uint32_t now_ms);
void neon_breaker_mx_draw(uint32_t now_ms);

typedef struct {
    uint8_t state;
    uint8_t level;
    uint8_t lives;
    uint8_t shield_charges;
    uint8_t magnet_charges;
    uint8_t slow_charges;
    uint8_t multi_charges;
    uint8_t overdrive_charges;
    uint8_t energy;
    uint8_t combo;
    uint16_t bricks_left;
    uint32_t score;
    uint32_t hi;
    uint8_t boss_active;
    uint8_t boss_hp;
    uint8_t fw, fh;
    uint8_t frame_rgb[299][3];
    uint32_t mirror_dirty[10];
} neon_breaker_status_t;

void neon_breaker_get_status(neon_breaker_status_t *out);

#ifdef __cplusplus
}
#endif
