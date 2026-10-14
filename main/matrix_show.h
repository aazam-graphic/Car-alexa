#pragma once

#include <stdbool.h>
#include <stdint.h>

bool matrix_show_open(uint32_t now_ms);
bool matrix_show_active(void);
bool matrix_show_route(uint16_t dig, uint16_t tap, uint32_t now_ms);
void matrix_show_tick(uint32_t now_ms);
void matrix_show_close(uint32_t now_ms);

uint8_t matrix_show_effect_index(void);
uint8_t matrix_show_effect_count(void);
const char *matrix_show_effect_name(uint8_t index);
const char *matrix_show_effect_description(uint8_t index);
bool matrix_show_auto_demo(void);

