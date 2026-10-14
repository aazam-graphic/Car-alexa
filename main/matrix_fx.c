/*
 * matrix_fx.c - particle engine for the 23x13 / 299 LED matrix shows.
 *
 * Fireworks: 4 bursts per ~2.7 s cycle (main / echo / pink / crackle).
 * Every burst sprays a fan of particles that:
 *   - fly outward with per-particle speed jitter (no identical spokes)
 *   - droop under gravity (willow look, they do NOT float in vacuum)
 *   - decay along a 3-sample trail behind the head
 *   - are rendered with bilinear splats -> sub-pixel smooth motion
 *   - are additively blended, so overlaps bloom like real light
 *
 * No malloc: fixed pool below. No per-pixel procedural loop: cost is
 * O(particles), not O(299 pixels x sqrt).
 */
#include "matrix_fx.h"
#include "led_matrix.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

#define FX_MAX_PARTICLE   56
#define FX_CYCLE_MS       2700u
#define FX_DRAG_X8        14u    /* 0.055 per step -> speed falls off */
#define FX_DRAG_Y8        11u
#define FX_GRAV_Y8        5u     /* +0.0195 px/step^2 droop */
#define FX_TRAIL1         0.30f  /* px behind head (fraction of 1 step) */
#define FX_TRAIL2         0.68f
#define FX_IGNITE         3u     /* frames of brightness ramp at launch    */
#define FX_SMEAR_FULL     1.2f   /* px/step at which the trail reaches 100% */

typedef struct {
    float    x, y;        /* sub-pixel position                        */
    float    vx, vy;
    uint16_t age, life;   /* steps                                     */
    uint16_t hue;
    uint8_t  kind;        /* FX_P_*                                    */
    uint8_t  active;
} fx_particle_t;

#define FX_P_SPARK 0
#define FX_P_WILL  1
#define FX_P_SNAKE 2

static fx_particle_t s_p[FX_MAX_PARTICLE];
static uint32_t s_slot;          /* which cycle we are in                 */
static uint32_t s_slot_age;      /* ms inside current cycle               */
static bool s_fired;             /* burst ignites on this step (for sfx)  */
static uint32_t s_rng;           /* per-burst rng stream (xorshift)       */

/* -------------------------------------------------------------------------- */
/* Shared colour helpers                                                      */
/* -------------------------------------------------------------------------- */
rgb_t hsv_rgb(uint16_t h, uint8_t sat, uint8_t val)
{
    h %= 360u;
    uint8_t region = (uint8_t)(h / 60u);
    uint8_t rem = (uint8_t)((h - (uint16_t)region * 60u) * 255u / 60u);
    uint8_t p = (uint8_t)((uint16_t)val * (255u - sat) / 255u);
    uint8_t q = (uint8_t)((uint16_t)val * (255u - (uint16_t)sat * rem / 255u) / 255u);
    uint8_t t = (uint8_t)((uint16_t)val * (255u - (uint16_t)sat * (255u - rem) / 255u) / 255u);
    switch (region) {
    case 0: return (rgb_t){val, t, p};
    case 1: return (rgb_t){q, val, p};
    case 2: return (rgb_t){p, val, t};
    case 3: return (rgb_t){p, q, val};
    case 4: return (rgb_t){t, p, val};
    default:return (rgb_t){val, p, q};
    }
}
void hsv_put(int x, int y, uint16_t h, uint8_t sat, uint8_t val)
{
    rgb_t c = hsv_rgb(h, sat, val);
    matrix_set_xy(x, y, c.r, c.g, c.b);
}

/* -------------------------------------------------------------------------- */
/* Deterministic rng (per-burst stream, no global state drift)                */
/* -------------------------------------------------------------------------- */
static uint32_t fx_rand(void)
{
    uint32_t x = s_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s_rng = x;
    return x;
}
/* 0.0 .. 1.0 */
static float fx_randf(void) { return (float)(fx_rand() >> 8) / 16777216.0f; }
/* -1.0 .. +1.0 */
static float fx_rands(void) { return (float)((int32_t)(fx_rand() >> 8) - 8388608) / 8388608.0f; }

/* ---- palettes: 5 families, each a hue base + spread ---- */
static uint16_t fx_hue_base(uint8_t pal) { return (uint16_t)(10u + pal * 66u); }

/* ---- spawn one burst of `kind` at (cx, cy) ---- */
static void fx_burst(float cx, float cy, uint8_t count, float speed,
                     uint8_t kind, uint8_t pal)
{
    uint16_t base = fx_hue_base(pal);
    for (uint8_t i = 0; i < count; i++) {
        int slot = -1;
        for (int k = 0; k < FX_MAX_PARTICLE; k++)
            if (!s_p[k].active) { slot = k; break; }
        if (slot < 0) break;                     /* pool full: skip, no malloc */

        fx_particle_t *p = &s_p[slot];
        float ang = (float)i * (2.0f * PI_F / (float)count) + fx_rands() * 0.30f;
        float sp = speed * (0.62f + 0.55f * fx_randf());
        p->x = cx; p->y = cy;
        /* slight upward bias so the fan opens like a real shell */
        p->vx = cosf(ang) * sp;
        p->vy = sinf(ang) * sp - speed * 0.16f;
        p->age = 0;
        p->life = kind == FX_P_WILL  ? (uint16_t)(46u + fx_rand() % 20u)
                : kind == FX_P_SNAKE ? (uint16_t)(30u + fx_rand() % 16u)
                                     : (uint16_t)(36u + fx_rand() % 20u);
        p->hue = (uint16_t)fmodf(base + fx_rands() * 26.0f + 360.0f, 360.0f);
        p->kind = kind;
        p->active = 1;
    }
}

/* ---- timeline: what fires when inside a cycle ----
   Offsets MUST be multiples of FX_STEP_MS or the burst is silently skipped
   (the sim only ever observes 0,30,60,...). Verified by host sim. */
static void fx_timeline(const int w, const int h)
{
    if (s_slot_age == 0u) {
        s_fired = true;
        float cx = 3.0f + fx_randf() * (float)(w - 7);
        float cy = 2.0f + fx_randf() * (float)(h - 5);
        fx_burst(cx, cy, 16, 0.60f, FX_P_SPARK, (uint8_t)(s_slot % 5u));
        /* faint willow overlay: droops lower, lives longer */
        fx_burst(cx, cy, 8, 0.42f, FX_P_WILL, (uint8_t)((s_slot + 2u) % 5u));
    }
    if (s_slot_age == 420u) {
        float cx = 3.0f + fx_randf() * (float)(w - 7);
        float cy = 2.0f + fx_randf() * (float)(h - 5);
        fx_burst(cx, cy, 6, 0.50f, FX_P_SPARK, (uint8_t)((s_slot + 3u) % 5u));
    }
    if (s_slot_age == 900u) {
        float cx = 3.0f + fx_randf() * (float)(w - 7);
        float cy = 2.0f + fx_randf() * (float)(h - 5);
        fx_burst(cx, cy, 12, 0.54f, FX_P_SNAKE, (uint8_t)((s_slot + 3u) % 5u));
    }
    if (s_slot_age == 1320u) {
        float cx = 3.0f + fx_randf() * (float)(w - 7);
        float cy = 2.0f + fx_randf() * (float)(h - 5);
        fx_burst(cx, cy, 7, 0.46f, FX_P_SPARK, (uint8_t)((s_slot + 4u) % 5u));
    }
    if (s_slot_age == 1680u) {
        /* big finale shell: fills the board, this is the "wow" frame */
        float cx = (float)(w - 1) * 0.5f + fx_rands() * 2.2f;
        float cy = (float)(h - 1) * 0.46f + fx_rands() * 1.4f;
        fx_burst(cx, cy, 22, 0.84f, FX_P_SPARK, (uint8_t)((s_slot + 1u) % 5u));
        fx_burst(cx, cy, 12, 0.56f, FX_P_WILL, (uint8_t)((s_slot + 4u) % 5u));
        fx_burst(cx, cy, 7, 1.02f, FX_P_SNAKE, (uint8_t)(s_slot % 5u));
    }
    if (s_slot_age == 2280u) {
        float cx = 3.0f + fx_randf() * (float)(w - 7);
        float cy = 2.0f + fx_randf() * (float)(h - 5);
        fx_burst(cx, cy, 9, 0.46f, FX_P_SPARK, (uint8_t)((s_slot + 2u) % 5u));
    }
}

void matrix_fx_reset(void)
{
    memset(s_p, 0, sizeof(s_p));
    s_slot = 0;
    s_slot_age = 0;
    s_fired = false;
    s_rng = 0x1f2e3d4cu ^ 0x9e3779b9u;
}

void matrix_fx_step(void)
{
    const int w = matrix_field_w(), h = matrix_field_h();
    s_fired = false;
    fx_timeline(w, h);

    for (int i = 0; i < FX_MAX_PARTICLE; i++) {
        fx_particle_t *p = &s_p[i];
        if (!p->active) continue;
        p->age++;
        if (p->age >= p->life) { p->active = 0; continue; }

        /* integrate (drag + gravity), fixed step = FX_STEP_MS */
        p->vx -= p->vx * (float)FX_DRAG_X8 / 256.0f;
        p->vy -= p->vy * (float)FX_DRAG_Y8 / 256.0f;
        p->vy += (float)FX_GRAV_Y8 / 256.0f;
        p->x += p->vx;
        p->y += p->vy;

        /* cull off-field: cheap out-of-bounds guard, no leaks in the pool */
        if (p->x < -3.0f || p->x > (float)w + 2.0f ||
            p->y < -3.0f || p->y > (float)h + 2.0f) p->active = 0;
    }

    s_slot_age += FX_STEP_MS;
    if (s_slot_age >= FX_CYCLE_MS) {
        s_slot_age = 0;
        s_slot++;
        /* reseed per cycle so bursts differ but stay reproducible */
        s_rng = (s_rng * 1664525u) + 1013904223u + s_slot;
        if (s_rng == 0u) s_rng = 0x1f2e3d4cu;
    }
}

/* ---- render: bilinear additive splat (the smooth-motion core) ---- */
static void fx_splat(float fx, float fy, uint16_t hue, uint8_t sat,
                     uint8_t val, float scale, int w, int h)
{
    if (val == 0u || scale <= 0.0f) return;
    rgb_t c = hsv_rgb(hue, sat, val);
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float ax = fx - (float)x0;
    float ay = fy - (float)y0;
    float cr = (float)c.r * scale, cg = (float)c.g * scale, cb = (float)c.b * scale;

    for (int dy = 0; dy <= 1; dy++) {
        int py = y0 + dy;
        if (py < 0 || py >= h) continue;
        float wy = dy ? ay : 1.0f - ay;
        for (int dx = 0; dx <= 1; dx++) {
            int px = x0 + dx;
            if (px < 0 || px >= w) continue;
            float wgt = (dx ? ax : 1.0f - ax) * wy * scale;
            if (wgt < 0.012f) continue;   /* below LED floor: skip write */
            matrix_add_xy(px, py,
                          (uint8_t)(cr * wgt), (uint8_t)(cg * wgt), (uint8_t)(cb * wgt));
        }
    }
}

void matrix_fx_render(int w, int h)
{
    for (int i = 0; i < FX_MAX_PARTICLE; i++) {
        fx_particle_t *p = &s_p[i];
        if (!p->active) continue;

        float t = (float)p->age / (float)p->life;
        /* brightness curve: bright head, embers linger (never a hard cut) */
        float fade = 1.0f - t;
        uint8_t v = (uint8_t)(255.0f * fade * (0.45f + 0.55f * fade));
        /* ignition ramp: a real shell takes ~100 ms to reach full brightness.
           Without this the core snaps 0->255 in a single frame, which is
           exactly the "sharp" artefact we are trying to remove. */
        if (p->age < FX_IGNITE)
            v = (uint8_t)((uint16_t)v * (uint16_t)(p->age + 1u) / (FX_IGNITE + 1u));
        if (v == 0u) continue;

        /* hue drifts as it cools: embers shift from white-hot to colour */
        uint16_t hue = (uint16_t)((p->hue + (uint16_t)(t * 40.0f)) % 360u);
        uint8_t sat = (uint8_t)(40u + (uint16_t)(t * 190.0f));   /* white -> saturated */

        if (p->kind == FX_P_SNAKE) {
            /* snake: hue cycles hard around the wheel, strobing look */
            hue = (uint16_t)((p->hue + (uint16_t)(p->age * 26u)) % 360u);
            sat = 235u;
            if ((p->age / 3u) & 1u) v = (uint8_t)((uint16_t)v * 42u / 100u);
        }

        /* trail strength follows speed: a nearly-stationary ember has no smear,
           and gating it stops head+trail from stacking into a saturated blob */
        float sp = sqrtf(p->vx * p->vx + p->vy * p->vy);
        float smear = sp * (1.0f / FX_SMEAR_FULL);
        if (smear > 1.0f) smear = 1.0f;

        fx_splat(p->x, p->y, hue, sat, v, 1.0f, w, h);
        /* trail: two sub-frame samples behind the head = motion smear */
        fx_splat(p->x - p->vx * FX_TRAIL1, p->y - p->vy * FX_TRAIL1,
                 hue, sat, (uint8_t)((float)v * 0.55f * smear), 1.0f, w, h);
        fx_splat(p->x - p->vx * FX_TRAIL2, p->y - p->vy * FX_TRAIL2,
                 hue, sat, (uint8_t)((float)v * 0.26f * smear), 1.0f, w, h);
    }
}

bool matrix_fx_burst_fired(void) { return s_fired; }

/* ---- background pass: night sky + stars (drawn by show before fx) ---- */
void matrix_fx_background(int x, int y, int w, int h, uint32_t t)
{
    /* vertical gradient: deep blue at top, slightly warmer at horizon */
    uint8_t v = (uint8_t)(4u + (uint16_t)y * 4u / (uint16_t)(h > 1 ? h - 1 : 1));
    uint16_t hue = (uint16_t)(228u - (uint16_t)y * 3u);

    /* sparse stars: deterministic per (x,y), twinkle on their own phase */
    uint32_t seed = (uint32_t)x * 0x45d9f3bu ^ (uint32_t)y * 0x119de1f3u;
    if ((seed & 0x3Fu) == 0x0Bu) {
        uint8_t tw = (uint8_t)(90u + 90u * (0.5f + 0.5f * sinf((float)t * 0.006f + (float)(seed & 7u))));
        v = tw;
        hue = (uint16_t)((seed >> 8) % 60u) + 200u;
    }
    hsv_put(x, y, hue, 150u, v);
    (void)w;
}
