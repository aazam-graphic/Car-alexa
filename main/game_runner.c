/*
 * NEON RUNNER — original side-scrolling cyber platform.
 * 23x13 / 299 LED matrix, 50 ms fixed tick, no dynamic memory in play.
 */
#include "game_runner.h"
#include "led_matrix.h"
#include "car_global.h"
#include "car_os.h"
#include "input_events.h"
#include "snd_bank.h"
#include "notif.h"
#include "runner_levels.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "matrix_show.h"
#include "game_serpent.h"
#include "game_neon_raid.h"
#include "game_neon_breaker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "runner";

#define R_W 23
#define R_H 13
#define R_TICK_MS 50
#define R_PLAYER_W 2
#define R_PLAYER_H 4
#define R_GRV  (8)   /* px/(tick^2), fixed-point x/8 */
#define R_TERM (40)  /* px/tick     X/8 */
#define R_JUMP (-24) /* px/tick      x/8 */
#define R_ACC   8
#define R_FRIC  6
#define R_MAX_VX   24
#define R_MAX_OBJECTS 64

typedef enum { RPT_NONE = 0, RPT_PLATFORM, RPT_SPIKE, RPT_ENERGY, RPT_ENEMY, RPT_EXIT, RPT_BLOCK } rpt_t;

typedef struct {
    uint8_t type;
    int16_t world_x;
    int8_t y;
    int8_t width;
    int8_t height;
    bool active;
} ro_t;

typedef struct {
    ro_t obj[R_MAX_OBJECTS];
    uint8_t count;
    uint8_t width;
} rw_t;

static rw_t s_world;
static runner_status_t s_st;
static runner_player_t s_p;
static uint32_t s_accum;
static uint32_t s_last_tick;
static uint32_t s_back_t0;
static uint32_t s_state_t0;
static uint8_t s_selected_level = 1;
static uint8_t s_unlocked = 1;
static uint8_t s_hi_unlocked = 1;
static uint32_t s_hi;
static uint16_t s_energy;
static uint32_t s_jump_buf;
static int16_t s_camera_x;
static bool s_pad_lost;
static bool s_rt_held;
static bool ledge_now(void);

static bool s_inited;
static uint8_t s_previous_orient;

/* -------------------------------------------------------------------------- */
/* Fixed-point helpers                                                        */
/* -------------------------------------------------------------------------- */
static int np(int16_t v) { return (int)(v >> 3); }

static bool rect_overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
    return (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
}

static bool solid(const ro_t *o) { return o->active && o->type == RPT_PLATFORM; }
static bool hazard(const ro_t *o) { return o->active && (o->type == RPT_SPIKE || o->type == RPT_ENEMY || o->type == RPT_BLOCK); }
static bool pickup(const ro_t *o) { return o->active && o->type == RPT_ENERGY; }
static bool exitp(const ro_t *o) { return o->active && o->type == RPT_EXIT; }

static void world_reset(uint8_t lv)
{
    if (lv < 1) lv = 1;
    if (lv > RUNNER_LEVEL_COUNT) lv = RUNNER_LEVEL_COUNT;
    const runner_level_t *L = &RUNNER_LEVELS[lv - 1];
    memset(&s_world, 0, sizeof(s_world));
    s_world.width = L->width;
    for (uint8_t i = 0; i < L->object_count; i++) {
        s_world.obj[s_world.count++] = (ro_t){
            .type = L->objects[i].type,
            .world_x = L->objects[i].world_x,
            .y = L->objects[i].y,
            .width = L->objects[i].width,
            .height = L->objects[i].height,
            .active = L->objects[i].active,
        };
    }
    s_p.x_fp = (int16_t)L->spawn_x * 8;
    s_p.y_fp = (int16_t)L->spawn_y * 8;
    s_p.vx_fp = 0;
    s_p.vy_fp = 0;
    s_p.grounded = true;
    s_p.alive = true;
    s_p.lives = 3;
    s_accum = 0;
    s_camera_x = s_p.x_fp / 8 - 8;
    if (s_camera_x < 0) s_camera_x = 0;
    s_jump_buf = 0;
    s_energy = 0;
}

static bool spawn_safe(const runner_level_t *L)
{
    int sx = np(L->spawn_x), sy = np(L->spawn_y);
    for (uint8_t i = 0; i < L->object_count; i++) {
        const runner_object_t *o = &L->objects[i];
        if (!o->active) continue;
        if (o->type == RUNNER_PLATFORM) {
            if (sx + R_PLAYER_W <= o->world_x || sx >= o->world_x + o->width) continue;
            if (sy + R_PLAYER_H <= o->y || sy >= o->y + o->height) continue;
            /* allow spawn directly above floor */
            if (o->y == sy + R_PLAYER_H) continue;
            return false;
        }
        if (o->type == RUNNER_SPIKE || o->type == RUNNER_ENEMY || o->type == RUNNER_MOVING_BLOCK) {
            if (sx < o->world_x + o->width && sx + R_PLAYER_W > o->world_x &&
                sy < o->y + o->height && sy + R_PLAYER_H > o->y) return false;
        }
    }
    return true;
}

static bool level_safe(const runner_level_t *L)
{
    if (L->object_count == 0 || L->width <= 0) return false;
    bool has_exit = false;
    bool has_path_to_exit = false;
    int max_x = L->width;
    for (uint8_t i = 0; i < L->object_count; i++) {
        const runner_object_t *o = &L->objects[i];
        if (o->width <= 0 || o->height <= 0) return false;
        if (o->world_x < 0 || o->y < 0 || o->world_x + o->width > max_x || o->y + o->height > R_H) return false;
        if (o->type == RUNNER_EXIT) has_exit = true;
        if (o->type == RUNNER_PLATFORM && o->y == 10) has_path_to_exit = true;
    }
    if (!has_exit) return false;
    if (!has_path_to_exit) return false;
    if (L->spawn_x < 0 || L->spawn_y < 0 || L->spawn_x >= max_x || L->spawn_y >= R_H) return false;
    if (L->exit_x < 0 || L->exit_y < 0 || L->exit_x >= max_x || L->exit_y >= R_H) return false;
    return spawn_safe(L);
}

/* lightweight validation; not full A*, but enough to reject bad data. */
static bool runner_levels_validate(void)
{
    bool ok = true;
    for (uint8_t i = 0; i < RUNNER_LEVEL_COUNT; i++) {
        ok = ok && level_safe(&RUNNER_LEVELS[i]);
    }
    return ok;
}

/* -------------------------------------------------------------------------- */
/* Collision                                                                  */
/* -------------------------------------------------------------------------- */

static bool landed_on(int next_y)
{
    int rx = np(s_p.x_fp);
    int ry = next_y >> 3;
    for (uint8_t i = 0; i < s_world.count; i++) {
        ro_t *o = &s_world.obj[i];
        if (!solid(o)) continue;
        if (rx + R_PLAYER_W <= o->world_x || rx >= o->world_x + o->width) continue;
        int top = o->y;
        int player_bottom_next = ry + R_PLAYER_H;
        if (player_bottom_next >= top &&
            (s_p.y_fp >> 3) + R_PLAYER_H <= top + 1) {
            s_p.y_fp = (int16_t)((top - R_PLAYER_H) * 8);
            s_p.vy_fp = 0;
            s_p.grounded = true;
            return true;
        }
    }
    return false;
}

static bool collect_any(void)
{
    int rx = np(s_p.x_fp);
    int ry = s_p.y_fp >> 3;
    for (uint8_t i = 0; i < s_world.count; i++) {
        ro_t *o = &s_world.obj[i];
        if (!pickup(o)) continue;
        if (rect_overlaps(rx, ry, R_PLAYER_W, R_PLAYER_H,
                          o->world_x, o->y, o->width, o->height)) {
            o->active = false;
            s_energy++;
            s_st.score += 10;
            snd_play("score_tick");
            return true;
        }
    }
    return false;
}

static bool hits_hazard(void)
{
    for (uint8_t i = 0; i < s_world.count; i++) {
        ro_t *o = &s_world.obj[i];
        if (!hazard(o)) continue;
        if (rect_overlaps(np(s_p.x_fp), s_p.y_fp >> 3, R_PLAYER_W, R_PLAYER_H,
                        o->world_x, o->y, o->width, o->height)) return true;
    }
    return false;
}
static bool reached_exit(void)
{
    for (uint8_t i = 0; i < s_world.count; i++) {
        ro_t *o = &s_world.obj[i];
        if (!exitp(o)) continue;
        if (rect_overlaps(np(s_p.x_fp), s_p.y_fp >> 3, R_PLAYER_W, R_PLAYER_H,
                        o->world_x, o->y, o->width, o->height)) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/* Physics step                                                               */
/* -------------------------------------------------------------------------- */
static void physics_step(const xbox360_pad_t *pad, uint16_t dig, uint16_t tap)
{
    /* D-pad deliberately unused. Left stick X + X/B face buttons steer. */
    bool left = (abs(pad->lx) > 7000 && pad->lx < 0) || (dig & B_X);
    bool right = (abs(pad->lx) > 7000 && pad->lx > 0) || (dig & B_B);
    if (left && !right) {
        if (s_p.vx_fp > -R_MAX_VX) s_p.vx_fp -= R_ACC;
        if (s_p.vx_fp < -R_MAX_VX) s_p.vx_fp = -R_MAX_VX;
    } else if (right && !left) {
        if (s_p.vx_fp < R_MAX_VX) s_p.vx_fp += R_ACC;
        if (s_p.vx_fp > R_MAX_VX) s_p.vx_fp = R_MAX_VX;
    } else {
        if (s_p.vx_fp > R_FRIC) s_p.vx_fp -= R_FRIC;
        else if (s_p.vx_fp < -R_FRIC) s_p.vx_fp += R_FRIC;
        else s_p.vx_fp = 0;
    }

    if ((tap & B_A) && s_p.grounded) {
        s_p.vy_fp = R_JUMP;
        s_p.grounded = false;
        s_jump_buf = 150;
        snd_play("ui_nav");
    }

    /* integrate x */
    int nx = np(s_p.x_fp + s_p.vx_fp);
    if (nx < 0) { nx = 0; s_p.vx_fp = 0; }
    if (nx + R_PLAYER_W > s_world.width) { nx = s_world.width - R_PLAYER_W; s_p.vx_fp = 0; }
    s_p.x_fp = (int16_t)(nx * 8);

    /* gravity */
    if (!s_p.grounded) {
        s_p.vy_fp += R_GRV;
        if (s_p.vy_fp > R_TERM) s_p.vy_fp = R_TERM;
    } else {
        /* local fall-off test; if no solid under feet, start falling */
        if (!landed_on(s_p.y_fp)) {
            s_p.grounded = false;
            s_p.vy_fp = 8;
        }
    }
    if (!s_p.grounded) {
        int ny = (s_p.y_fp >> 3) + (s_p.vy_fp >> 3);
        int ny_fp = (ny << 3);
        if (ny_fp < 0) { ny_fp = 0; s_p.vy_fp = 0; }
        /* now try landing */
        int old_b = (s_p.y_fp >> 3) + R_PLAYER_H;
        s_p.y_fp = (int16_t)ny_fp;
        int new_b = (s_p.y_fp >> 3) + R_PLAYER_H;
        if (landed_on(s_p.y_fp)) {
            if (new_b == old_b) {
                /* already supported */
            }
        } else {
            if (new_b >= R_H + 3) {
                s_p.alive = false;
            }
        }
    }

    /* walked off a platform */
    if (s_p.grounded) {
        int rx = np(s_p.x_fp);
        int under_y = (s_p.y_fp >> 3) + R_PLAYER_H;
        bool support = false;
        for (uint8_t i = 0; i < s_world.count; i++) {
            ro_t *o = &s_world.obj[i];
            if (!solid(o)) continue;
            if (rx + R_PLAYER_W <= o->world_x || rx >= o->world_x + o->width) continue;
            if (under_y == o->y) { support = true; break; }
        }
        if (!support) {
            s_p.grounded = false;
            s_p.vy_fp = 8;
        }
    }

    if (hits_hazard()) s_p.alive = false;
    if (collect_any()) {}

    /* camera: player near center, never <0, stops at end */
    int px = np(s_p.x_fp) + R_PLAYER_W / 2;
    int target = px - (R_W / 2);
    if (target < 0) target = 0;
    int max_cam = s_world.width - R_W;
    if (max_cam < 0) max_cam = 0;
    if (target > max_cam) target = max_cam;
    if (target > s_camera_x) s_camera_x = target;
}

/* -------------------------------------------------------------------------- */
/* Render                                                                     */
/* -------------------------------------------------------------------------- */
static void draw_rect(int sx, int sy, int w, int h, rgb_t c)
{
    for (int y = sy; y < sy + h; y++)
        for (int x = sx; x < sx + w; x++)
            if (x >= 0 && x < R_W && y >= 0 && y < R_H)
                matrix_set_xy_rgb(x, y, c);
}

static void draw_pixel(int sx, int sy, rgb_t c)
{
    if (sx >= 0 && sx < R_W && sy >= 0 && sy < R_H)
        matrix_set_xy_rgb(sx, sy, c);
}

static void runner_render(void)
{
    matrix_clear();
    /* small background */
    for (int i = 0; i < 12; i++) {
        int x = (i * 7 + (s_camera_x % 8)) % R_W;
        int y = (i * 3) % 10;
        draw_pixel(x, y, (rgb_t){10, 20, 40});
    }
    for (uint8_t i = 0; i < s_world.count; i++) {
        ro_t *o = &s_world.obj[i];
        if (!o->active) continue;
        int sx = o->world_x - s_camera_x;
        int sy = o->y;
        if (sx + o->width < 0 || sx >= R_W) continue;
        switch (o->type) {
        case RPT_PLATFORM:
            draw_rect(sx, sy, o->width, o->height, (rgb_t){40, 220, 160});
            break;
        case RPT_SPIKE:
            draw_rect(sx, sy, o->width, o->height, (rgb_t){255, 40, 40});
            break;
        case RPT_ENERGY:
            draw_rect(sx, sy, 1, 1, (rgb_t){255, 220, 80});
            break;
        case RPT_ENEMY:
            draw_rect(sx, sy, o->width, o->height, (rgb_t){190, 80, 255});
            break;
        case RPT_EXIT:
            draw_rect(sx, sy, o->width, o->height, (rgb_t){0, 200, 255});
            break;
        case RPT_BLOCK:
            draw_rect(sx, sy, o->width, o->height, (rgb_t){255, 140, 20});
            break;
        default: break;
        }
    }
    /* player */
    int px = np(s_p.x_fp) - s_camera_x;
    int py = s_p.y_fp >> 3;
    draw_rect(px, py, R_PLAYER_W, R_PLAYER_H, s_p.alive ? (rgb_t){70, 255, 160} : (rgb_t){120, 40, 40});

    /* HUD: y 11..12 */
    for (int y = 11; y <= 12; y++)
        for (int x = 0; x < R_W; x++)
            draw_pixel(x, y, (rgb_t){5, 5, 10});
    /* lives: cyan dots */
    for (uint8_t i = 0; i < s_p.lives && i < 3; i++)
        draw_pixel(1 + i * 2, 11, (rgb_t){80, 255, 120});
    /* energy as yellow dots up to 10 */
    for (uint8_t i = 0; i < s_energy && i < 10; i++)
        draw_pixel(8 + i, 11, (rgb_t){255, 220, 80});
    /* level dots */
    for (uint8_t i = 0; i < s_selected_level && i < 5; i++)
        draw_pixel(1 + i * 2, 12, (rgb_t){80, 160, 255});

    if (s_st.state == RUNNER_OFF || s_st.state == RUNNER_MENU) {
        draw_rect(1, 2, 21, 9, (rgb_t){30, 40, 60});
        /* "NEO" in 3x5 dots */
        static const uint8_t N[] = {0x7, 0x9, 0xD, 0x9, 0x9};
        static const uint8_t E[] = {0xF, 0x8, 0xE, 0x8, 0xF};
        static const uint8_t O[] = {0xE, 0x9, 0x9, 0x9, 0xE};
        const uint8_t *g[] = {N, E, O};
        int x0 = 4;
        for (int ci = 0; ci < 3; ci++) {
            for (int ry = 0; ry < 5; ry++) {
                uint8_t row = g[ci][ry];
                for (int cx = 0; cx < 4; cx++)
                    if (row & (1 << (3 - cx)))
                        draw_pixel(x0 + cx, 3 + ry, (rgb_t){60, 255, 140});
            }
            x0 += 5;
        }
        return;
    }
    if (s_st.state == RUNNER_GAME_OVER) {
        draw_rect(1, 3, 21, 8, (rgb_t){60, 20, 20});
        for (int x = 3; x < 20; x++) draw_pixel(x, 4, (rgb_t){255, 80, 80});
    }
    if (s_st.state == RUNNER_PAUSED) {
        draw_pixel(10, 5, (rgb_t){255, 220, 80});
        draw_pixel(12, 5, (rgb_t){255, 220, 80});
    }
    matrix_show();
}

/* -------------------------------------------------------------------------- */
/* NVS                                                                        */
/* -------------------------------------------------------------------------- */
static void save_stats(void)
{
    nvs_handle_t h;
    if (nvs_open("runner", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_st.score > s_hi) s_hi = s_st.score;
    nvs_set_u32(h, "hsi", s_hi);
    nvs_set_u8(h, "unlocked", s_unlocked);
    nvs_set_u8(h, "his", s_hi_unlocked);
    nvs_commit(h);
    nvs_close(h);
}

static void load_stats(void)
{
    nvs_handle_t h;
    if (nvs_open("runner", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v;
    uint8_t b;
    if (nvs_get_u32(h, "hsi", &v) == ESP_OK) s_hi = v;
    if (nvs_get_u8(h, "unlocked", &b) == ESP_OK && b >= 1 && b <= RUNNER_LEVEL_COUNT) s_unlocked = b;
    if (nvs_get_u8(h, "his", &b) == ESP_OK && b >= 1 && b <= RUNNER_LEVEL_COUNT) s_hi_unlocked = b;
    nvs_close(h);
}

/* -------------------------------------------------------------------------- */
/* Public                                                                     */
/* -------------------------------------------------------------------------- */
void runner_init(void)
{
    if (s_inited) return;
    s_inited = true;
    memset(&s_world, 0, sizeof(s_world));
    memset(&s_st, 0, sizeof(s_st));
    memset(&s_p, 0, sizeof(s_p));
    s_st.state = RUNNER_OFF;
    s_st.score = 0;
    s_st.level = 1;
    s_st.lives = 3;
    s_st.energy = 0;
    s_st.player_x = 0;
    s_st.player_y = 0;
    s_st.camera_x = 0;
    s_st.grounded = true;
    s_energy = 0;
    s_hi = 0;
    s_unlocked = 1;
    s_hi_unlocked = 1;
    s_previous_orient = 0;
    load_stats();
    bool ok = runner_levels_validate();
    ESP_LOGI(TAG, "runner_init hi=%lu unlocked=%u levels_ok=%d",
             (unsigned long)s_hi, s_unlocked, ok);
}

bool runner_active(void)
{
    return s_st.state != RUNNER_OFF;
}

uint8_t runner_state(void)
{
    return (uint8_t)s_st.state;
}

bool runner_open(void)
{
    runner_init();
    if (!led_matrix_ready() || g.estop) return false;
    if (!os_parked()) return false;
    if (serpent_arcade_state() != 0 ||
        neon_raid_state() != 0 ||
        neon_breaker_state() != 0 ||
        matrix_show_active()) return false;
    if (!led_matrix_acquire(OWNER_ARCADE)) return false;
    s_previous_orient = matrix_get_orient();
    matrix_set_orient(MATRIX_ORIENT_LANDSCAPE);
    led_matrix_set_demo(false);
    led_matrix_set_dim(false);
    matrix_set_dim_pct(100);
    input_flush_context_switch();
    s_st.state = RUNNER_MENU;
    s_state_t0 = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_selected_level = s_unlocked;
    s_back_t0 = 0;
    snd_play("ui_open_sheet");
    notif_push(NOTIF_RULES, "NEON RUNNER", s_state_t0);
    return true;
}

static void exit_full(uint32_t now)
{
    s_st.state = RUNNER_OFF;
    s_st.player_x = 0;
    s_st.player_y = 0;
    save_stats();
    input_consume_all();
    matrix_set_dim_pct(100);
    led_matrix_safe_off();
    matrix_set_orient(s_previous_orient);
    led_matrix_release(OWNER_ARCADE);
    led_matrix_set_demo(true);
    snd_play("ui_back");
    os_ctx_t *ctx = car_os_ctx();
    if (ctx && ctx->state == OS_GAME_6) os_request_screen(ctx, OS_GAMES_HUB, now);
}

bool runner_route(const xbox360_pad_t *pad, uint16_t dig,
                  uint16_t tap, uint32_t now)
{
    runner_init();
    if (s_st.state == RUNNER_OFF) return false;

    if (tap & B_GUIDE) return false;

    if (dig & B_BACK) {
        if (!s_back_t0) s_back_t0 = now;
        if (now - s_back_t0 >= 1000u) { exit_full(now); return false; }
    } else {
        s_back_t0 = 0;
    }

    /* player disconnect: park + pause */
    if (!pad || !pad->present) {
        if (s_st.state == RUNNER_PLAYING || s_st.state == RUNNER_COUNTDOWN) {
            s_st.state = RUNNER_PAUSED;
            s_state_t0 = now;
            s_pad_lost = true;
            led_matrix_set_dim(true);
        }
        return true;
    }
    if (s_pad_lost) {
        s_pad_lost = false;
        led_matrix_set_dim(false);
        input_flush_context_switch();
        return true;
    }

    /* BACK tap goes back to internal menu (hold already handled above) */
    if ((tap & B_BACK) && s_st.state != RUNNER_MENU) {
        s_st.state = RUNNER_MENU;
        s_state_t0 = now;
        s_rt_held = false;
        snd_play("ui_back");
        return true;
    }

    switch (s_st.state) {
    case RUNNER_MENU:
        if (tap & B_LB) { if (s_selected_level > 1) s_selected_level--; snd_play("ui_nav"); }
        if (tap & B_RB) { if (s_selected_level < s_unlocked) s_selected_level++; snd_play("ui_nav"); }
        if (tap & B_START) {
            world_reset(s_selected_level);
            s_st.level = s_selected_level;
            s_st.lives = s_p.lives;
            s_st.score = 0;
            s_st.energy = 0;
            s_st.state = RUNNER_COUNTDOWN;
            s_state_t0 = now;
            snd_play("profile_chime_game");
        }
        break;
    case RUNNER_COUNTDOWN:
        if (tap & B_START) { s_st.state = RUNNER_PAUSED; s_state_t0 = now; snd_play("ui_open_sheet"); }
        break;
    case RUNNER_PLAYING:
        if (tap & B_START) { s_st.state = RUNNER_PAUSED; s_state_t0 = now; matrix_set_dim_pct(35); snd_play("ui_open_sheet"); }
        physics_step(pad, dig, tap);
        if (!s_p.alive) {
            s_st.state = RUNNER_DYING;
            s_state_t0 = now;
            snd_play("game_over");
        }
        if (reached_exit()) {
            s_st.state = RUNNER_LEVEL_CLEAR;
            s_state_t0 = now;
            s_st.score += 100;
            s_st.score += (((int16_t)s_p.lives) * 50);
            snd_play("highscore_fanfare");
        }
        break;
    case RUNNER_PAUSED:
        if (tap & B_START) {
            s_st.state = (s_p.alive ? RUNNER_PLAYING : RUNNER_GAME_OVER);
            s_state_t0 = now;
            matrix_set_dim_pct(100);
            snd_play("ui_close_sheet");
        }
        break;
    case RUNNER_DYING:
        if (now - s_state_t0 >= 1200u) {
            if (s_p.lives > 1) {
                s_p.lives--;
                world_reset(s_selected_level);
                s_st.state = RUNNER_COUNTDOWN;
                s_state_t0 = now;
            } else {
                s_st.state = RUNNER_GAME_OVER;
                s_state_t0 = now;
                save_stats();
            }
        }
        break;
    case RUNNER_LEVEL_CLEAR:
        if (now - s_state_t0 >= 1300u) {
            uint8_t next = s_selected_level + 1;
            if (next > RUNNER_LEVEL_COUNT) next = RUNNER_LEVEL_COUNT;
            if (next > s_unlocked) s_unlocked = next;
            s_selected_level = next;
            world_reset(next);
            s_st.level = next;
            s_st.state = RUNNER_COUNTDOWN;
            s_state_t0 = now;
        }
        break;
    case RUNNER_GAME_OVER:
        if ((tap & B_START) && now - s_state_t0 > 700u) {
            s_st.score = 0;
            world_reset(s_selected_level);
            s_st.level = s_selected_level;
            s_st.state = RUNNER_COUNTDOWN;
            s_state_t0 = now;
            snd_play("ui_tap");
        }
        break;
    default:
        s_st.state = RUNNER_MENU;
        break;
    }
    return true;
}

void runner_tick(uint32_t now)
{
    if (s_st.state == RUNNER_OFF) return;
    static uint32_t s_last = 0;
    if (now - s_last < 33u) return;
    s_last = now;

    switch (s_st.state) {
    case RUNNER_MENU:
        if (now - s_state_t0 < 2000u) s_state_t0 = now;
        runner_render();
        break;
    case RUNNER_COUNTDOWN:
        if (now - s_state_t0 >= 1500u) {
            s_st.state = RUNNER_PLAYING;
            s_state_t0 = now;
            s_p.vy_fp = 0;
            s_p.grounded = true;
            matrix_set_dim_pct(100);
        }
        runner_render();
        break;
    case RUNNER_PLAYING:
        if (ledge_now()) {
            s_accum += (now - s_last_tick);
            if (s_accum > 200) s_accum = 200;
            s_last_tick = now;
            while (s_accum >= R_TICK_MS) {
                s_accum -= R_TICK_MS;
                /* physics runs from route; timing kept by s_accum */
            }
        }
        runner_render();
        break;
    case RUNNER_PAUSED:
    case RUNNER_DYING:
    case RUNNER_LEVEL_CLEAR:
    case RUNNER_GAME_OVER:
        runner_render();
        break;
    default: break;
    }

    /* TFT companion snapshot bookkeeping */
    s_st.state = s_st.state;
    s_st.score = s_st.score;
    s_st.level = s_selected_level;
    s_st.lives = s_p.lives;
    s_st.energy = s_energy;
    s_st.player_x = np(s_p.x_fp);
    s_st.player_y = s_p.y_fp >> 3;
    s_st.camera_x = s_camera_x;
    s_st.grounded = s_p.grounded;
}

static bool ledge_now(void) { return true; }

void runner_get_status(runner_status_t *out)
{
    if (!out) return;
    *out = s_st;
}

/* registry stubs */
void runner_mx_init(void) { runner_init(); }
void runner_mx_update(const xbox360_pad_t *pad, uint32_t now_ms) { (void)pad; (void)now_ms; }
void runner_mx_draw(uint32_t now_ms) { (void)now_ms; }
