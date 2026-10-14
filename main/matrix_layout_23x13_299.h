/*
 * matrix_layout_23x13_299.h
 *
 * Custom physical LED layout for the ESP32-S3 Car OS.
 *
 * Physical WS2812 chain:
 *   LED 1 DIN <- ESP32 data output
 *   LED 1 DOUT -> LED 2 DIN -> LED 3 DIN -> ... -> LED 299 DIN
 *
 * Total physical LEDs: 299
 * Visible logical grid: 23 columns x 13 rows
 *
 * IMPORTANT:
 * - Drawing labels are 1..299.
 * - C LED-buffer indexes are 0..298.
 * - buffer_index = drawing_label - 1.
 * - This is NOT row-major and NOT standard serpentine wiring.
 * - `matrix_led_label[y][x]` maps visual/logical coordinates to
 *   the real physical LED label in the WS2812 data chain.
 * - Only the central matrix driver should include/use this map.
 * - Games must draw only by logical x/y coordinates.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Matrix geometry                                                            */
/* -------------------------------------------------------------------------- */

#define MATRIX_WIDTH               23u
#define MATRIX_HEIGHT              13u
#define MATRIX_LED_COUNT           299u
#define MATRIX_LED_LABEL_MIN       1u
#define MATRIX_LED_LABEL_MAX       MATRIX_LED_COUNT

#define MATRIX_INVALID_LED_INDEX   UINT16_MAX

#if ((MATRIX_WIDTH * MATRIX_HEIGHT) != MATRIX_LED_COUNT)
#error "Matrix geometry must be exactly 23 x 13 = 299 LEDs"
#endif

/* -------------------------------------------------------------------------- */
/* Physical location lookup table                                             */
/* -------------------------------------------------------------------------- */

static const uint16_t matrix_led_label[MATRIX_HEIGHT][MATRIX_WIDTH] = {
    /* y = 0 */
    {
          1, 299,   2, 274,   3, 273,   4, 248,
          5, 247,   6, 222,   7, 221,   8, 196,
          9, 195,  10, 170,  11, 169,  12
    },

    /* y = 1 */
    {
         24, 298,  23, 275,  22, 272,  21, 249,
         20, 246,  19, 223,  18, 220,  17, 197,
         16, 194,  15, 171,  14, 168,  13
    },

    /* y = 2 */
    {
         25, 297,  26, 276,  27, 271,  28, 250,
         29, 245,  30, 224,  31, 219,  32, 198,
         33, 193,  34, 172,  35, 167,  36
    },

    /* y = 3 */
    {
         48, 296,  47, 277,  46, 270,  45, 251,
         44, 244,  43, 225,  42, 218,  41, 199,
         40, 192,  39, 173,  38, 166,  37
    },

    /* y = 4 */
    {
         49, 295,  50, 278,  51, 269,  52, 252,
         53, 243,  54, 226,  55, 217,  56, 200,
         57, 191,  58, 174,  59, 165,  60
    },

    /* y = 5 */
    {
         72, 294,  71, 279,  70, 268,  69, 253,
         68, 242,  67, 227,  66, 216,  65, 201,
         64, 190,  63, 175,  62, 164,  61
    },

    /* y = 6 */
    {
         73, 293,  74, 280,  75, 267,  76, 254,
         77, 241,  78, 228,  79, 215,  80, 202,
         81, 189,  82, 176,  83, 163,  84
    },

    /* y = 7 */
    {
         96, 292,  95, 281,  94, 266,  93, 255,
         92, 240,  91, 229,  90, 214,  89, 203,
         88, 188,  87, 177,  86, 162,  85
    },

    /* y = 8 */
    {
         97, 291,  98, 282,  99, 265, 100, 256,
        101, 239, 102, 230, 103, 213, 104, 204,
        105, 187, 106, 178, 107, 161, 108
    },

    /* y = 9 */
    {
        120, 290, 119, 283, 118, 264, 117, 257,
        116, 238, 115, 231, 114, 212, 113, 205,
        112, 186, 111, 179, 110, 160, 109
    },

    /* y = 10 */
    {
        121, 289, 122, 284, 123, 263, 124, 258,
        125, 237, 126, 232, 127, 211, 128, 206,
        129, 185, 130, 180, 131, 159, 132
    },

    /* y = 11 */
    {
        144, 288, 143, 285, 142, 262, 141, 259,
        140, 236, 139, 233, 138, 210, 137, 207,
        136, 184, 135, 181, 134, 158, 133
    },

    /* y = 12 */
    {
        145, 287, 146, 286, 147, 261, 148, 260,
        149, 235, 150, 234, 151, 209, 152, 208,
        153, 183, 154, 182, 155, 157, 156
    },
};

/* -------------------------------------------------------------------------- */
/* Coordinate / label / buffer-index conversion                               */
/* -------------------------------------------------------------------------- */

static inline bool matrix_xy_is_valid(uint8_t x, uint8_t y)
{
    return x < MATRIX_WIDTH && y < MATRIX_HEIGHT;
}

static inline uint16_t matrix_xy_to_led_label(uint8_t x, uint8_t y)
{
    if (!matrix_xy_is_valid(x, y)) {
        return 0;
    }

    return matrix_led_label[y][x];
}

static inline uint16_t matrix_led_label_to_index(uint16_t label)
{
    if (label < MATRIX_LED_LABEL_MIN || label > MATRIX_LED_LABEL_MAX) {
        return MATRIX_INVALID_LED_INDEX;
    }

    return (uint16_t)(label - 1u);
}

static inline uint16_t matrix_xy_to_led_index(uint8_t x, uint8_t y)
{
    const uint16_t label = matrix_xy_to_led_label(x, y);

    if (label == 0u) {
        return MATRIX_INVALID_LED_INDEX;
    }

    return matrix_led_label_to_index(label);
}

static inline bool matrix_led_label_to_xy(uint16_t label,
                                          uint8_t *out_x,
                                          uint8_t *out_y)
{
    if (label < MATRIX_LED_LABEL_MIN || label > MATRIX_LED_LABEL_MAX ||
        out_x == NULL || out_y == NULL) {
        return false;
    }

    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
            if (matrix_led_label[y][x] == label) {
                *out_x = x;
                *out_y = y;
                return true;
            }
        }
    }

    return false;
}

static inline bool matrix_layout_validate(void)
{
    bool seen[MATRIX_LED_COUNT] = { false };

    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
            const uint16_t label = matrix_led_label[y][x];

            if (label < MATRIX_LED_LABEL_MIN ||
                label > MATRIX_LED_LABEL_MAX) {
                return false;
            }

            const uint16_t index = (uint16_t)(label - 1u);

            if (seen[index]) {
                return false;
            }

            seen[index] = true;
        }
    }

    for (uint16_t index = 0; index < MATRIX_LED_COUNT; index++) {
        if (!seen[index]) {
            return false;
        }
    }

    return true;
}

#ifdef __cplusplus
}
#endif
