/*
 * falak_show.c - FALAK name + beating heart (idle show, unique style).
 *
 * 6 s BEATING HEART (red gradient + pink top glow + gold sparkles +
 *     cyan ECG line that spikes on every beat)
 * 6 s SCROLLING NAME (gold "FALAK" 3x5 marquee with red shadow + dim border)
 *
 * Static data only, no malloc, caller throttles to ~30 FPS.
 */
#include "falak_show.h"
#include "led_matrix.h"

/* 5x7 font (readable on strip), rows top->bottom, bit4 = left pixel */
static const uint8_t F_F[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
static const uint8_t F_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
static const uint8_t F_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
static const uint8_t F_K[7] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11};

static const uint8_t *glyph(char c)
{
    if (c == 'F') return F_F;
    if (c == 'A') return F_A;
    if (c == 'L') return F_L;
    if (c == 'K') return F_K;
    return NULL;   /* space / unknown */
}
#define GLYPH_W 5
#define GLYPH_H 7
#define GLYPH_ADV 6   /* 5 px + 1 space */

/* heart 9x7 bitmap, bit8 = left pixel */
static const uint16_t HEART[7] = {
    0b011000110,
    0b111111111,
    0b111111111,
    0b011111110,
    0b001111100,
    0b000111000,
    0b000010000,
};
#define HEART_W 9
#define HEART_H 7
#define HEART_Y 1
/* HEART_X centres on the active field width */
#define HEART_X ((int)(matrix_field_w() - HEART_W) / 2)

static void draw_heart(uint8_t glow)
{
    /* glow 0..255 from beat envelope: brightness + pink top */
    for (int r = 0; r < HEART_H; r++) {
        uint16_t bits = HEART[r];
        for (int c = 0; c < HEART_W; c++) {
            if (!(bits & (1u << (HEART_W - 1 - c)))) continue;
            /* vertical gradient: top pink -> deep red bottom */
            uint8_t pr = 220, pg = 10, pb = 30;
            if (r <= 1) { pr = 255; pg = 110; pb = 140; }
            else if (r <= 3) { pr = 240; pg = 40; pb = 70; }
            uint8_t b = (uint8_t)(90 + (glow * 165 / 255));
            matrix_set_xy(HEART_X + c, HEART_Y + r,
                          (uint8_t)(pr * b / 255),
                          (uint8_t)(pg * b / 255),
                          (uint8_t)(pb * b / 255));
        }
    }
}

static void draw_sparkles(uint32_t now)
{
    /* 3 deterministic twinkles around the heart */
    for (int i = 0; i < 3; i++) {
        uint32_t h = (now / 300) * 37 + i * 101;
        int fw = matrix_field_w(), fh = matrix_field_h();
        int x = (int)(h % (uint32_t)fw);
        int y = (int)(((h / 12 + i * 3) % (uint32_t)(fh - 1)));
        /* keep sparkles outside the heart body */
        int hx = x - HEART_X, hy = y - HEART_Y;
        if (hx >= 0 && hx < HEART_W && hy >= 0 && hy < HEART_H &&
            (HEART[hy] & (1u << (HEART_W - 1 - hx)))) continue;
        uint8_t tw = (uint8_t)(((now / 4 + i * 80) % 255) > 130 ? 255 : 40);
        matrix_set_xy(x, y, (uint8_t)(tw * 200 / 255),
                      (uint8_t)(tw * 170 / 255), (uint8_t)(tw * 90 / 255));
    }
}

static void draw_ecg(uint32_t now, bool beat)
{
    /* cyan baseline bottom row, spike sweeping + tall spike on beat */
    int fw = matrix_field_w(), fh = matrix_field_h();
    int erow = fh - 1;
    int sweep = (int)((now / 70) % (uint32_t)fw);
    for (int x = 0; x < fw; x++) {
        int d = (sweep - x + fw) % fw;
        uint8_t tail = d < 4 ? (uint8_t)(120 - d * 30) : 25;
        matrix_set_xy(x, erow, 0, (uint8_t)(tail + 20), (uint8_t)(tail + 60));
    }
    /* spike column */
    if (erow - 1 >= 0) matrix_set_xy(sweep, erow - 1, 0, 220, 255);
    matrix_set_xy(sweep, erow, 0, 255, 255);
    if (beat) {
        if (erow - 2 >= 0) matrix_set_xy(sweep, erow - 2, 0, 200, 255);
        if (erow - 3 >= 0) matrix_set_xy(sweep, erow - 3, 0, 140, 220);
    }
}

static void heart_mode(uint32_t now)
{
    /* beat every 900 ms, thump 220 ms */
    uint32_t t = now % 900;
    bool beat = t < 220;
    uint8_t glow = beat ? (uint8_t)(255 - t * 120 / 220) : 60;
    matrix_clear();
    draw_heart(glow);
    draw_sparkles(now);
    draw_ecg(now, beat);
    matrix_show();
}

static void name_mode(uint32_t now)
{
    static const char TXT[] = "FALAK";
    const int fw = matrix_field_w();
    const int total = 5 * GLYPH_ADV - 1;    /* 29 */
    const int span = total + fw;
    /* slow scroll + hold when centred */
    int step = (int)((now / 170) % (span + 8));
    int off = step - fw;
    if (off < -fw) off = -fw;
    if (off > total) off = total;
    /* centre-hold for readability */
    int centre = (fw - total) / 2;
    if (off >= centre - 2 && off <= centre + 2) off = centre;
    matrix_clear();
    /* bright gold rows 1..7, no border/shadow (clean) */
    for (int i = 0; TXT[i]; i++) {
        const uint8_t *gl = glyph(TXT[i]);
        if (!gl) continue;
        for (int r = 0; r < GLYPH_H; r++) {
            for (int c = 0; c < GLYPH_W; c++) {
                if (!(gl[r] & (1u << (GLYPH_W - 1 - c)))) continue;
                int x = off + i * GLYPH_ADV + c, y = 1 + r;
                if (x >= 0 && x < fw)
                    matrix_set_xy(x, y, 255, 170, 20);   /* bright gold */
            }
        }
    }
    uint8_t prev_br = matrix_get_brightness();
    matrix_set_brightness(MATRIX_MAX_BRIGHTNESS);   /* full cap for text */
    matrix_show();
    matrix_set_brightness(prev_br);
}

void falak_tick(uint32_t now_ms)
{
    /* 6 s heart / 6 s name loop */
    if (((now_ms / 6000) & 1) == 0) heart_mode(now_ms);
    else name_mode(now_ms);
}
