/*
 * led_matrix.h - 120-LED RGB ARCADE MATRIX (blueprint STEP 3+4).
 *
 * Hardware: 12x10 = 120 WS2812B, serpentine/zig-zag, DIN -> GPIO18.
 * Room/roof WS2812 (roof_light.c) test ke liye NIKALA gaya hai taaki
 * GPIO18 + ek RMT channel free ho (S3 par RMT channels limited hain).
 *
 * Power (blueprint 3.3, MUST follow):
 *   5V 8A min (10A recommended), injection start+end, common GND,
 *   1000uF cap at LED1, 330-470ohm data resistor, level-shifter 74AHCT125.
 *   Kabhi ESP32 USB se 120 LEDs power mat do.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- takeover switch: 1 = matrix owns GPIO18, roof_light yields ---- */
#define LED_MATRIX_TAKEOVER_ROOF  1

#define LED_MATRIX_GPIO   18
#define LED_MATRIX_W      12
#define LED_MATRIX_H      10
#define LED_MATRIX_COUNT  (LED_MATRIX_W * LED_MATRIX_H)  /* 120 */

/* brightness safety cap (blueprint 3.4): 72/255 ~= 28% */
#define MATRIX_MAX_BRIGHTNESS 72

typedef struct { uint8_t r, g, b; } rgb_t;

#define COL_OFF    ((rgb_t){0, 0, 0})
#define COL_BORDER ((rgb_t){0, 12, 28})
#define COL_CYAN   ((rgb_t){0, 110, 180})
#define COL_HEAD   ((rgb_t){80, 255, 255})
#define COL_GOLD   ((rgb_t){255, 150, 0})
#define COL_RED    ((rgb_t){220, 0, 0})
#define COL_PURPLE ((rgb_t){130, 0, 180})
#define COL_GREEN  ((rgb_t){0, 180, 35})
#define COL_WHITE  ((rgb_t){100, 100, 100})

void led_matrix_init(void);          /* RMT + OFF, idempotent */
void led_matrix_deinit(void);        /* clear + free RMT (roof wapas chahiye to) */
bool led_matrix_ready(void);
bool led_matrix_is_active(void);     /* takeover active? (roof_light yield check) */

/* XY mapper (blueprint 4): x=0..11 left->right, y=0..9 top->bottom */
int16_t matrix_xy_to_index(int16_t x, int16_t y);

/* frame API: sirf mapper se likho, raw index mat use karo */
void matrix_set_xy(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b);
void matrix_set_xy_rgb(int16_t x, int16_t y, rgb_t c);
void matrix_clear(void);
void matrix_show(void);              /* brightness-scaled push, max ~30 FPS caller pacing */

/* brightness 0..72 (cap enforced). default 50 */
void matrix_set_brightness(uint8_t b);
uint8_t matrix_get_brightness(void);
/* percent 1..100 mapped onto the cap (100% = full cap) */
void matrix_set_brightness_pct(uint8_t pct);

/* orientation: LANDSCAPE 12x10, PORTRAIT 10x12 (vertical strip).
   Portrait = exact transpose remap, zero crop, all 120 LEDs. */
#define MATRIX_ORIENT_LANDSCAPE 0
#define MATRIX_ORIENT_PORTRAIT  1
void matrix_set_orient(uint8_t o);
uint8_t matrix_get_orient(void);
/* active field size (set by orientation) */
uint8_t matrix_field_w(void);
uint8_t matrix_field_h(void);

/* idle show (boot test ke baad): 0 FALAK 1 SWEEP 2 OFF */
#define MATRIX_IDLE_FALAK 0
#define MATRIX_IDLE_SWEEP 1
#define MATRIX_IDLE_OFF   2
void matrix_set_idle(uint8_t m);
uint8_t matrix_get_idle(void);

/* safety (blueprint 13): GUIDE/disconnect/reboot paths */
void led_matrix_safe_off(void);      /* immediate clear + show */
void led_matrix_set_dim(bool dim);   /* disconnect: 10% + red border pulse */
/* ownership: FALAK idle show vs ARCADE game (never both draw) */
typedef enum { OWNER_FALAK = 0, OWNER_ARCADE, OWNER_DIAG } matrix_owner_t;
bool led_matrix_acquire(matrix_owner_t o);   /* false if taken */
void led_matrix_release(matrix_owner_t o);   /* back to FALAK idle */
matrix_owner_t led_matrix_owner(void);

/* current limiter: mA estimate of the frame after brightness cap.
   matrix_show() auto-scales down when > 1500 mA. */
uint16_t matrix_estimate_ma(void);
/* extra dim multiplier % (pause = 25). 100 = off. */
void matrix_set_dim_pct(uint8_t pct);
/* short goodbye flash after arcade exit (visible even if idle == OFF) */
void led_matrix_exit_flash(void);

/* ---- Phase-B mapping diagnostics (STEP 4) ---- */
void matrix_test_border(void);       /* rectangular border -> zig-zag check */
void matrix_test_diagonal(void);     /* (0,0)->(9,9) diagonal */
void matrix_test_rgb5(void);         /* Phase-A: pehle 5 LEDs R/G/B @20 */

/* demo tick: boot self-test + idle border sweep. car_task se har loop call karo.
   30 FPS internally throttled, zero malloc, zero block. */
void led_matrix_tick(uint32_t now_ms);
void led_matrix_set_demo(bool on);   /* test ke liye ON, game aaye to OFF */

#ifdef __cplusplus
}
#endif
