#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xbox360.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Matrix arcade game: 23x13 logical grid / 299 physical LEDs. */
void neon_raid_init(void);
bool neon_raid_active(void);
bool neon_raid_open(void);
bool neon_raid_route(const xbox360_pad_t *pad, uint16_t dig,
                     uint16_t tap, uint32_t now_ms);
void neon_raid_tick(uint32_t now_ms);
uint8_t neon_raid_state(void);
bool neon_raid_suppress_oled(void);

/* Games-hub registry hooks. The real game runs in neon_raid_route/tick. */
void neon_raid_mx_init(void);
void neon_raid_mx_update(const xbox360_pad_t *pad, uint32_t now_ms);
void neon_raid_mx_draw(uint32_t now_ms);

typedef struct {
    uint8_t state;
    uint32_t score;
    uint32_t hi;
    uint8_t level;
    uint8_t lives;
    uint8_t hp;
    uint8_t energy;
    uint8_t combo;
    uint16_t kills;
    uint8_t objective;
    uint8_t objective_now;
    uint8_t objective_target;
    uint8_t weapon;
    uint16_t boss_hp;
    uint8_t fw, fh;
    uint8_t frame_rgb[299][3];
    uint32_t mirror_dirty[10];
} neon_raid_status_t;

void neon_raid_get_status(neon_raid_status_t *out);

#ifdef __cplusplus
}
#endif
