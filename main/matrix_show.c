/* 20 full-colour procedural shows for the 23x13 / 299 LED matrix. */
#include "matrix_show.h"
#include "matrix_fx.h"
#include "led_matrix.h"
#include "car_os.h"
#include "input_events.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>

#define SHOW_COUNT 20
/* 30 FPS target. car loop runs at 20 ms while a matrix show is active, so a
   30 ms cadence is reachable (2 of every 3 loop iterations) and gives every
   show a stable, jitter-free frame budget. The particle sim uses this as its
   fixed step, so physics stays identical at any caller rate. */
#define SHOW_FRAME_MS 30u
#define SHOW_AUTO_MS  6500u
#define PI_F 3.14159265358979323846f

/* phosphor afterglow: keep a fraction of the previous frame so fast-moving
   LEDs leave a trail instead of snapping on/off (LEDs have no motion blur,
   the eye supplies it only if brightness decays gradually). */
#define SHOW_GHOST_KEEP 168u   /* ~0.66 per frame => ~4 frame tail */
#define SHOW_GHOST_MIN  3u     /* below this it is indistinguishable from off */

static const char *const s_names[SHOW_COUNT] = {
    "Rainbow Ribbon", "Aurora Veil", "Peacock Dance", "Fireworks", "Starfield Warp",
    "Plasma Dreams", "Digital Rain", "Living Flame", "Ocean Tide", "Dancing Butterfly",
    "Blooming Flower", "Spiral Galaxy", "Meteor Shower", "Heart Pulse", "Neon Orbit",
    "Electric Storm", "Confetti Party", "Rainbow Falls", "Firefly Garden", "Kaleidoscope"
};
static const char *const s_desc[SHOW_COUNT] = {
    "A flowing rainbow wave across every pixel", "Northern lights ripple in emerald and violet",
    "A peacock opens a shimmering jewel tail", "Bright colour bursts bloom and fade",
    "Stars rush past in a deep-space tunnel", "Living neon clouds blend and swirl",
    "Glowing data streams race down the matrix", "A layered flame dances from the base",
    "Turquoise waves roll over a deep blue sea", "Symmetric wings gently open and close",
    "A radiant flower opens petal by petal", "Colourful arms spin through a galaxy",
    "Meteor trails sweep across the night sky", "A pulsing heart glows in shifting colour",
    "Rings of neon light expand and orbit", "Electric bolts flash through a storm",
    "Bright confetti drifts through a party", "A cascading rainbow waterfall shimmers",
    "Warm fireflies glow in a tiny garden", "Mirrored geometry turns into a prism"
};

static uint8_t s_index;
static bool s_active, s_auto;
static uint8_t s_old_orient;
static uint8_t s_old_brightness;
static uint32_t s_last_frame, s_last_auto;

/* per-pixel afterglow buffer, indexed by LOGICAL x/y (never by LED index, so
   the physical mapping table stays the single source of truth in the driver) */
#define GHOST_W LED_MATRIX_W
#define GHOST_H LED_MATRIX_H
static uint8_t s_ghost[GHOST_H][GHOST_W][3];

/* renderer task: 30 FPS pacing independent of the car loop jitter */
static TaskHandle_t s_task;
static SemaphoreHandle_t s_fb_lock;   /* framebuffer is single-owner */
static uint8_t s_fx_blip;              /* varies the burst launch pitch */

static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

static inline rgb_t hsv(uint16_t h, uint8_t sat, uint8_t val)
{
    return hsv_rgb(h, sat, val);
}

static inline void put(int x, int y, uint16_t h, uint8_t sat, uint8_t val)
{
    hsv_put(x, y, h, sat, val);
}

static void draw_peacock(int x, int y, uint32_t t)
{
    float sway = sinf((float)t * 0.0014f) * 0.20f;
    float spread = 0.78f + 0.20f * (0.5f + 0.5f * sinf((float)t * 0.0018f));
    float dx = (float)x - (11.0f + sinf((float)t * 0.0014f));
    float dy = 12.6f - (float)y;
    float r = sqrtf(dx * dx + dy * dy);
    float a = atan2f(dx, dy) - sway;
    if (r >= 2.0f && r < 13.6f && fabsf(a) < spread) {
        int feather = (int)((a + spread) * 9.5f / spread);
        float phase = (float)t * 0.003f + feather * 0.65f;
        float eye_r = 9.3f + 0.55f * sinf(phase);
        float eye_ang = (a + spread) * 9.5f - floorf((a + spread) * 9.5f);
        uint8_t v = (uint8_t)(85 + 95 * (0.5f + 0.5f * sinf(r * 0.85f - phase)));
        uint16_t hue = (uint16_t)(150 + feather * 16 + (uint32_t)t / 24u);
        if (fabsf(r - eye_r) < 1.5f && (eye_ang < 0.22f || eye_ang > 0.78f)) {
            put(x, y, hue + 30, 220, 250);
        } else if (fabsf(r - eye_r) < 2.5f) {
            put(x, y, hue + 175, 220, 225);
        } else {
            put(x, y, hue, 230, v);
        }
    } else if (fabsf(dx) < 1.2f && y >= 7 && y <= 11) {
        put(x, y, 42, 205, 210);
    } else if (y == 8 && fabsf(dx) < 2.4f) {
        put(x, y, 52, 190, 255);
    } else if ((hash32((uint32_t)x * 31u + (uint32_t)y * 113u) & 127u) == ((t / 100u) & 127u)) {
        put(x, y, (uint16_t)(180 + x * 5), 120, 90);
    }
}

static void draw_pixel(uint8_t fx, int x, int y, uint32_t t)
{
    const int w = matrix_field_w(), h = matrix_field_h();
    float xf = (float)x, yf = (float)y;
    uint32_t seed = hash32((uint32_t)x * 0x45d9f3bu ^ (uint32_t)y * 0x119de1f3u);
    switch (fx) {
    case 0: /* rainbow ribbon */
        put(x, y, (uint16_t)(xf * 10.0f + yf * 18.0f + t / 12u), 255,
            (uint8_t)(150 + 100 * (0.5f + 0.5f * sinf(yf * 0.7f - (float)t * 0.004f))));
        break;
    case 1: { /* aurora veil */
        float wave = 5.5f + 3.4f * sinf(xf * 0.32f + (float)t * 0.0011f)
                   + 1.5f * sinf(xf * 0.77f - (float)t * 0.0007f);
        float band = fabsf(yf - wave);
        uint8_t v = band < 4.8f ? (uint8_t)(235.0f - band * 34.0f) : 8;
        put(x, y, (uint16_t)(118 + 58 * (0.5f + 0.5f * sinf(xf * 0.2f + t * 0.001f))), 220, v);
        break;
    }
    case 2: draw_peacock(x, y, t); break;
    case 3: /* fireworks: night-sky base only, particles drawn additively
               after the per-pixel pass by matrix_fx_render() */
        matrix_fx_background(x, y, w, h, t);
        break;
    case 4: { /* starfield warp */
        uint8_t v = 3;
        uint16_t hue = 205;
        for (uint32_t i = 0; i < 36; i++) {
            uint32_t sh = hash32(i * 977u + 19u);
            int sx = (int)(sh % (uint32_t)w);
            uint32_t speed = 1u + ((sh >> 9) & 3u);
            int sy = (int)(((sh >> 16) + t / (80u / speed)) % (uint32_t)h);
            if (x == sx && y == sy) { v = 90 + (uint8_t)((t / 7u + i * 23u) % 165u); hue = (uint16_t)(175 + i * 5); }
            else if (x == sx && y == (sy - 1 + h) % h) v = 30;
        }
        put(x, y, hue, 170, v);
        break;
    }
    case 5: { /* plasma clouds */
        float z = sinf(xf * 0.42f + t * 0.002f) + sinf(yf * 0.65f - t * 0.0017f)
                + sinf((xf + yf) * 0.31f + t * 0.0013f);
        put(x, y, (uint16_t)(190 + (z + 3.0f) * 42.0f + t / 24u), 245,
            (uint8_t)(125 + (z + 3.0f) * 21.0f));
        break;
    }
    case 6: { /* digital rain */
        uint32_t speed = 3u + (seed & 7u);
        int head = (int)(((seed >> 8) + t / (75u / (speed > 7 ? 7 : speed))) % (uint32_t)(h + 7));
        int d = head - y;
        uint8_t v = d == 0 ? 255 : (d > 0 && d < 5 ? (uint8_t)(175 - d * 32) : 2);
        put(x, y, (uint16_t)(112 + ((seed >> 19) % 35u)), 250, v);
        break;
    }
    case 7: { /* living flame */
        float flicker = sinf(xf * 1.7f + t * 0.009f) + sinf(xf * 0.53f - t * 0.004f)
                      + ((float)((seed + t / 37u) & 15u) / 15.0f - 0.5f);
        float heat = ((float)(h - 1 - y) / (float)h) * 225.0f + flicker * 45.0f;
        put(x, y, (uint16_t)(5 + heat * 0.17f), 255, (uint8_t)(heat < 0 ? 0 : heat > 255 ? 255 : heat));
        break;
    }
    case 8: { /* ocean tide */
        float crest = 6.0f + 2.7f * sinf(xf * 0.34f + t * 0.0018f)
                    + 1.0f * sinf(xf * 0.73f - t * 0.001f);
        float d = fabsf(yf - crest);
        uint8_t v = d < 1.0f ? 245 : (uint8_t)(105 - (d > 7 ? 7 : d) * 12);
        put(x, y, (uint16_t)(190 + 25 * sinf(xf * 0.15f + t * 0.001f)), 230, v);
        break;
    }
    case 9: { /* dancing butterfly */
        float dx = fabsf(xf - (w - 1) * 0.5f);
        float dy = yf - h * 0.52f;
        float flap = 0.58f + 0.42f * (0.5f + 0.5f * sinf(t * 0.0027f));
        float wing = dx * (1.25f + dy * 0.20f) / flap;
        if (wing < 5.3f && dy > -4.6f && dy < 4.2f) {
            uint16_t hue = (uint16_t)(275 + wing * 20 + t / 32u);
            uint8_t v = ((int)(wing * 2.0f + dy) % 3 == 0) ? 255 : 185;
            put(x, y, hue, 235, v);
        } else if (dx < 0.65f && dy > -4.0f && dy < 4.0f) put(x, y, 45, 150, 235);
        else put(x, y, 240, 150, 3);
        break;
    }
    case 10: { /* blooming flower */
        float dx = xf - (w - 1) * 0.5f, dy = yf - (h - 1) * 0.5f;
        float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx) + t * 0.0007f;
        float petal = fabsf(cosf(a * 5.0f));
        float bloom = 2.0f + 4.0f * (0.5f + 0.5f * sinf(t * 0.0016f));
        if (r < bloom * (0.28f + 0.72f * petal)) put(x, y, (uint16_t)(300 + a * 28 + t / 30u), 230, 235);
        else if (r < 1.7f) put(x, y, 48, 210, 255);
        else put(x, y, 285, 190, 3);
        break;
    }
    case 11: { /* spiral galaxy */
        float dx = (xf - (w - 1) * 0.5f) * 1.0f, dy = yf - (h - 1) * 0.5f;
        float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
        float arm = sinf(a * 2.0f - r * 0.85f + t * 0.0016f);
        uint8_t v = r < 1.4f ? 255 : (arm > 0.72f ? (uint8_t)(210 - r * 10) : 8);
        put(x, y, (uint16_t)(240 + r * 22 + t / 35u), 230, v);
        break;
    }
    case 12: { /* meteor shower */
        uint8_t v = 2; uint16_t hue = 190;
        for (uint32_t i = 0; i < 5; i++) {
            uint32_t sh = hash32(0x51f15e5u + i * 73u);
            int sx = (int)((sh + t / (35u + i * 11u)) % (uint32_t)(w + h));
            int sy = (int)(((sh >> 12) + t / (52u + i * 9u)) % (uint32_t)(h + 6));
            for (int tail = 0; tail < 4; tail++) {
                if (x == (sx - tail + w + h) % w && y == (sy - tail + h + 6) % h) {
                    v = (uint8_t)(250 - tail * 52); hue = (uint16_t)(28 + i * 52);
                }
            }
        }
        put(x, y, hue, 225, v);
        break;
    }
    case 13: { /* heartbeat */
        float pulse = 1.0f + 0.10f * sinf(t * 0.006f);
        float nx = (xf - (w - 1) * 0.5f) / (5.2f * pulse);
        float ny = -((yf - h * 0.52f) / (4.8f * pulse));
        float xx = nx * nx, yy = ny * ny;
        float eq = (xx + yy - 1.0f) * (xx + yy - 1.0f) * (xx + yy - 1.0f) - xx * ny * ny * ny;
        if (eq <= 0.03f) put(x, y, (uint16_t)(330 + xf * 6 + t / 22u), 235, 255);
        else put(x, y, 330, 230, 2);
        break;
    }
    case 14: { /* neon orbit */
        float dx = xf - (w - 1) * 0.5f, dy = (yf - (h - 1) * 0.5f) * 1.55f;
        float r = sqrtf(dx * dx + dy * dy), a = atan2f(dy, dx);
        float orbit = 2.4f + 2.2f * (0.5f + 0.5f * sinf(a * 3.0f + t * 0.002f));
        uint8_t v = fabsf(r - orbit) < 0.8f ? 245 : 4;
        put(x, y, (uint16_t)(185 + a * 35 + t / 16u), 250, v);
        break;
    }
    case 15: { /* electric storm */
        uint32_t strike = t / 430u;
        uint32_t sh = hash32(strike * 17u + 9u);
        int bolt_x = (int)(sh % (uint32_t)w);
        int bolt_y = (int)((sh >> 8) % (uint32_t)h);
        int zig = bolt_x + ((y / 2 + (int)(strike & 3u)) % 3 - 1);
        bool bolt = ((t % 430u) < 90u) && y <= bolt_y && x == zig;
        uint8_t flash = (t % 430u) < 45u ? 35 : 3;
        if (bolt) put(x, y, (uint16_t)(185 + strike * 31u), 120, 255);
        else put(x, y, 215, 230, flash);
        break;
    }
    case 16: { /* confetti party */
        uint8_t v = 3; uint16_t hue = 0;
        for (uint32_t i = 0; i < 34; i++) {
            uint32_t sh = hash32(i * 1187u + 77u);
            int sx = (int)(sh % (uint32_t)w);
            int speed = 2 + (int)((sh >> 8) & 5u);
            int sy = (int)(((sh >> 16) + t / (90u / (uint32_t)speed)) % (uint32_t)h);
            if (x == (sx + (int)(t / 400u + i) % 3) % w && y == sy) { v = 245; hue = (uint16_t)((sh >> 4) + t / 18u); }
        }
        put(x, y, hue, 255, v);
        break;
    }
    case 17: { /* rainbow falls */
        float ripple = sinf(xf * 0.48f + t * 0.0015f) + sinf(xf * 0.91f - t * 0.0022f);
        uint16_t hue = (uint16_t)(xf * 15 + ripple * 38 + t / 15u);
        uint8_t v = (uint8_t)(115 + 130 * (0.5f + 0.5f * sinf(yf * 0.8f + t * 0.004f)));
        put(x, y, hue, 245, v);
        break;
    }
    case 18: { /* firefly garden */
        uint8_t v = (y == h - 1 || (x % 7 == 2 && y > h / 2)) ? 28 : 2;
        uint16_t hue = 105;
        for (uint32_t i = 0; i < 12; i++) {
            uint32_t sh = hash32(i * 997u + 301u);
            int fx = (int)(sh % (uint32_t)w), fy = (int)((sh >> 8) % (uint32_t)(h - 2));
            int bob = (int)((t / (180u + i * 19u) + i) % 3u) - 1;
            if (x == fx && y == fy + bob) { v = (uint8_t)(140 + (hash32(t / 270u + i) & 115u)); hue = (uint16_t)(38 + i * 14); }
        }
        put(x, y, hue, 225, v);
        break;
    }
    default: { /* mirrored kaleidoscope */
        int sx = x < w / 2 ? x : w - 1 - x;
        int sy = y < h / 2 ? y : h - 1 - y;
        float d = (float)(sx * 3 + sy * 5);
        float pulse = sinf(d * 0.65f + t * 0.002f);
        put(x, y, (uint16_t)(d * 18 + t / 18u), 245, (uint8_t)(110 + 145 * (0.5f + 0.5f * pulse)));
        break;
    }
    }
}

/* -------------------------------------------------------------------------- */
/* Frame pipeline                                                             */
/* -------------------------------------------------------------------------- */

/* afterglow: out = max(new, decayed old), written back into the ghost buffer.
   This is what turns 1-pixel snapping into smooth streaks. */
static void show_ghost_pass(int w, int h)
{
    if (w > GHOST_W || h > GHOST_H) return;   /* portrait: skip, no room */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint8_t nr, ng, nb;
            if (!matrix_get_xy(x, y, &nr, &ng, &nb)) continue;
            uint8_t *g = s_ghost[y][x];
            uint8_t dr = (uint8_t)(((uint16_t)g[0] * SHOW_GHOST_KEEP) / 255u);
            uint8_t dg = (uint8_t)(((uint16_t)g[1] * SHOW_GHOST_KEEP) / 255u);
            uint8_t db = (uint8_t)(((uint16_t)g[2] * SHOW_GHOST_KEEP) / 255u);
            uint8_t or_ = nr > dr ? nr : dr;
            uint8_t og_ = ng > dg ? ng : dg;
            uint8_t ob_ = nb > db ? nb : db;
            if (or_ < SHOW_GHOST_MIN) or_ = 0;
            if (og_ < SHOW_GHOST_MIN) og_ = 0;
            if (ob_ < SHOW_GHOST_MIN) ob_ = 0;
            g[0] = or_; g[1] = og_; g[2] = ob_;
            matrix_set_xy(x, y, or_, og_, ob_);
        }
    }
}

/* The frame gate lives INSIDE the lock: both the renderer task and the car
   loop call this, so the check+advance must be atomic or one 30 ms step could
   be simulated twice (physics would run at 60 Hz while rendering at 30). */
static void show_frame_locked(uint32_t now)
{
    if (!s_active) return;
    if ((uint32_t)(now - s_last_frame) < SHOW_FRAME_MS) return;
    s_last_frame = now;
    if (s_auto && (uint32_t)(now - s_last_auto) >= SHOW_AUTO_MS) {
        s_index = (uint8_t)((s_index + 1u) % SHOW_COUNT);
        s_last_auto = now;
    }
    int w = matrix_field_w(), h = matrix_field_h();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) draw_pixel(s_index, x, y, now);
    if (s_index == FX_FIREWORKS) {
        matrix_fx_step();            /* fixed-step physics, exactly 30 FPS */
        matrix_fx_render(w, h);      /* additive, sub-pixel splats          */
        if (matrix_fx_burst_fired()) {
            /* launch thump: low synth blip, no blocking, no new assets */
            car_sfx_blip(s_fx_blip ? (uint16_t)(150u + s_fx_blip * 35u) : 150u);
            s_fx_blip = (uint8_t)((s_fx_blip + 1u) % 4u);
        }
    }
    show_ghost_pass(w, h);
    matrix_show();
}

static void show_render_frame(uint32_t now)
{
    if (s_fb_lock) xSemaphoreTake(s_fb_lock, portMAX_DELAY);
    show_frame_locked(now);
    if (s_fb_lock) xSemaphoreGive(s_fb_lock);
}

/* renderer task: fixed 30 FPS pacing, independent of car-loop jitter. Runs the
   same frame path as the car loop, so behaviour is identical whether or not it
   exists (the task is only a pacer; the car loop still owns input/state). */
static void show_task_fn(void *arg)
{
    (void)arg;
    TickType_t prev = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&prev, pdMS_TO_TICKS(SHOW_FRAME_MS));
        if (!s_active) continue;
        show_render_frame((uint32_t)(esp_timer_get_time() / 1000));
    }
}

bool matrix_show_active(void) { return s_active; }
uint8_t matrix_show_effect_index(void) { return s_index; }
uint8_t matrix_show_effect_count(void) { return SHOW_COUNT; }
const char *matrix_show_effect_name(uint8_t i) { return s_names[i % SHOW_COUNT]; }
const char *matrix_show_effect_description(uint8_t i) { return s_desc[i % SHOW_COUNT]; }
bool matrix_show_auto_demo(void) { return s_auto; }

bool matrix_show_open(uint32_t now)
{
    if (s_active || !led_matrix_ready() || !os_parked() || !led_matrix_acquire(OWNER_SHOW)) return false;
    s_old_orient = matrix_get_orient();
    s_old_brightness = matrix_get_brightness();
    matrix_set_orient(MATRIX_ORIENT_LANDSCAPE);
    led_matrix_set_demo(false);
    led_matrix_set_dim(false);
    matrix_set_dim_pct(100);
    matrix_set_brightness_pct(100);
    memset(s_ghost, 0, sizeof(s_ghost));
    matrix_fx_reset();
    s_fx_blip = 0;
    s_active = true;
    s_last_frame = now - SHOW_FRAME_MS;
    s_last_auto = now;
    input_flush_context_switch();

    /* 30 FPS pacer: created once, parks itself while the show is closed */
    if (!s_fb_lock) s_fb_lock = xSemaphoreCreateMutex();
    if (!s_task && xTaskCreate(show_task_fn, "m_show", 4096, NULL, 3, &s_task) != pdPASS)
        s_task = NULL;
    return true;
}

void matrix_show_close(uint32_t now)
{
    if (!s_active) return;
    s_active = false;
    s_auto = false;
    if (s_fb_lock) xSemaphoreTake(s_fb_lock, portMAX_DELAY);
    memset(s_ghost, 0, sizeof(s_ghost));
    matrix_clear();
    matrix_show();
    matrix_set_orient(s_old_orient);
    matrix_set_brightness(s_old_brightness);
    led_matrix_release(OWNER_SHOW);
    led_matrix_set_demo(true);
    if (s_fb_lock) xSemaphoreGive(s_fb_lock);
    input_consume_all();
    os_ctx_t *ctx = car_os_ctx();
    if (ctx && ctx->state == OS_MATRIX_SHOW) os_request_screen(ctx, OS_HOME, now);
}

bool matrix_show_route(uint16_t dig, uint16_t tap, uint32_t now)
{
    if (!s_active) return false;
    if (tap & B_GUIDE) return false; /* global E-stop remains authoritative */
    if (g.estop) { matrix_show_close(now); return true; }

    if (tap & (B_B | B_BACK)) { matrix_show_close(now); return true; }
    if (tap & (B_DLEFT | B_LB)) {
        s_index = (uint8_t)((s_index + SHOW_COUNT - 1u) % SHOW_COUNT);
        s_last_auto = now;
        car_sfx_blip(900);
    } else if (tap & (B_DRIGHT | B_RB)) {
        s_index = (uint8_t)((s_index + 1u) % SHOW_COUNT);
        s_last_auto = now;
        car_sfx_blip(1100);
    }
    if (tap & B_A) { s_auto = !s_auto; s_last_auto = now; car_sfx_click(); }
    if (tap & B_X) { s_index = (uint8_t)(esp_random() % SHOW_COUNT); s_last_auto = now; car_sfx_score(); }
    if (tap & B_START) { s_auto = !s_auto; s_last_auto = now; car_sfx_click(); }
    (void)dig;
    return true;
}

void matrix_show_tick(uint32_t now)
{
    show_render_frame(now);
}
