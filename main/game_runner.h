#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "xbox360.h"
#include "runner_status.h"

void runner_init(void);
bool runner_open(void);
bool runner_active(void);
bool runner_route(const xbox360_pad_t *pad, uint16_t dig,
                  uint16_t tap, uint32_t now_ms);
void runner_tick(uint32_t now_ms);
uint8_t runner_state(void);
void runner_get_status(runner_status_t *out);

/* Matrix arcade registry hooks, same shape as serpent/neon games. */
void runner_mx_init(void);
void runner_mx_update(const xbox360_pad_t *pad, uint32_t now_ms);
void runner_mx_draw(uint32_t now_ms);
