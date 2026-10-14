/*
 * NEON BREAKER: CORE WAR
 *
 * 299 physical LEDs, 23x13 logical matrix.
 * The game only uses logical x/y and the central matrix driver performs
 * the custom physical mapping.
 *
 * D-pad is intentionally ignored everywhere in gameplay and menu.
 *
 * Controls:
 *   Left Stick : paddle movement
 *   A          : launch / serve
 *   B          : shield
 *   X          : magnet paddle
 *   Y          : multi-ball
 *   LB         : slow-time
 *   RB         : overdrive
 *   LT         : charge cannon; release to fire
 *   RT         : laser while held
 *   START      : start / pause / resume / retry
 *   BACK       : menu / exit (hold exits)
 */
#include "game_neon_breaker.h"
#include "led_matrix.h"
#include "car_global.h"
#include "car_os.h"
#include "input_events.h"
#include "snd_bank.h"
#include "notif.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "neon_breaker";

#define NB_W                  23
#define NB_H                  13
#define NB_CELLS              299
#define NB_MAX_BRICKS         56   /* 8 columns x 7 rows */
#define NB_MAX_BALLS           6
#define NB_MAX_POWERUPS        8
#define NB_MAX_SHOTS           8
#define NB_MAX_PARTICLES      36
#define NB_BRICK_COLS          7
#define NB_BRICK_ROWS          7
#define NB_BRICK_W             2
#define NB_BRICK_X0            1
#define NB_BRICK_Y0            1
#define NB_PADDLE_Y           12
#define NB_FRAME_MS            33
#define NB_SIM_MS              16
#define NB_BACK_HOLD_MS     1000u
#define NB_SERVE_DELAY_MS    500u
#define NB_LEVEL_FX_MS       900u
#define NB_BOSS_MINI_HP       18u
#define NB_BOSS_FINAL_HP      45u
#define NB_MAX_ENERGY         100u

#define NB_IDX(x,y) ((y) * s_fw + (x))

/* -------------------------------------------------------------------------- */
/* Data                                                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    NB_OFF = 0,
    NB_MENU,
    NB_READY,
    NB_PLAY,
    NB_PAUSED,
    NB_CLEAR,
    NB_OVER
} nb_state_t;

typedef enum {
    BRICK_NORMAL = 0,
    BRICK_ARMOR,
    BRICK_EXPLOSIVE,
    BRICK_POWER,
    BRICK_METAL
} nb_brick_kind_t;

typedef enum {
    PU_WIDE = 0,
    PU_SHIELD,
    PU_MAGNET,
    PU_SLOW,
    PU_MULTI,
    PU_ENERGY,
    PU_BONUS,
    PU_OVERDRIVE,
    PU_COUNT
} nb_powerup_t;

typedef enum {
    SHOT_CANNON = 0,
    SHOT_LASER
} nb_shot_kind_t;

typedef enum {
    NB_LAYOUT_SOLID = 0,
    NB_LAYOUT_STRIPES,
    NB_LAYOUT_CHECKER,
    NB_LAYOUT_PYRAMID,
    NB_LAYOUT_DIAMOND,
    NB_LAYOUT_FORTRESS,
    NB_LAYOUT_COLUMNS,
    NB_LAYOUT_ZIGZAG,
    NB_LAYOUT_INVADER,
    NB_LAYOUT_CROSS,
    NB_LAYOUT_MINIBOSS,
    NB_LAYOUT_GATES,
    NB_LAYOUT_HOLE,
    NB_LAYOUT_FRAME,
    NB_LAYOUT_DENSE,
    NB_LAYOUT_STAGGER,
    NB_LAYOUT_CORE,
    NB_LAYOUT_FINAL
} nb_layout_t;

typedef struct {
    uint8_t kind;
    uint8_t hp;
    uint8_t max_hp;
    bool alive;
    uint8_t col;
    uint8_t row;
} nb_brick_t;

typedef struct {
    bool active;
    int16_t x8;            /* Q8.8 logical cell position */
    int16_t y8;
    int16_t vx8;
    int16_t vy8;
    uint8_t power_hits;    /* fireball-style pierce count */
    bool fireball;
    bool stuck;
} nb_ball_t;

typedef struct {
    bool active;
    int16_t x8;
    int16_t y8;
    uint8_t type;
} nb_powerup_inst_t;

typedef struct {
    bool active;
    int16_t x8;
    int16_t y8;
    int16_t vy8;
    uint8_t kind;
    uint8_t ttl;
} nb_shot_t;

typedef struct {
    bool active;
    int8_t x;
    int8_t y;
    int8_t dx;
    int8_t dy;
    uint8_t life;
    uint8_t kind;
} nb_particle_t;

typedef struct {
    bool active;
    int16_t x8;
    int16_t y8;
    int16_t vy8;
} nb_hazard_t;

typedef struct {
    uint8_t rows;
    uint8_t hp_scale;
    uint8_t layout;
    uint8_t special_rate;
    uint16_t ball_speed;
    uint8_t moving;
    uint8_t boss;
    uint8_t boss_hp;
} nb_level_cfg_t;

static const nb_level_cfg_t s_levels[20] = {
    {3,1, NB_LAYOUT_SOLID,    10,  54,0,0,0},
    {3,1, NB_LAYOUT_STRIPES,  12,  56,0,0,0},
    {4,1, NB_LAYOUT_CHECKER,  14,  58,0,0,0},
    {4,1, NB_LAYOUT_PYRAMID,  16,  60,0,0,0},
    {4,1, NB_LAYOUT_DIAMOND,  18,  62,0,0,0},
    {5,2, NB_LAYOUT_FORTRESS, 18,  64,0,0,0},
    {5,2, NB_LAYOUT_COLUMNS,  20,  66,0,0,0},
    {5,2, NB_LAYOUT_ZIGZAG,   21,  68,0,0,0},
    {5,2, NB_LAYOUT_INVADER,  22,  70,1,0,0},
    {4,2, NB_LAYOUT_MINIBOSS, 24,  72,1,1,NB_BOSS_MINI_HP},
    {5,2, NB_LAYOUT_GATES,    24,  74,1,0,0},
    {5,2, NB_LAYOUT_HOLE,     25,  76,1,0,0},
    {6,2, NB_LAYOUT_CROSS,    26,  78,1,0,0},
    {6,2, NB_LAYOUT_FRAME,    27,  80,1,0,0},
    {6,2, NB_LAYOUT_DENSE,    28,  82,1,0,0},
    {6,2, NB_LAYOUT_STAGGER,  29,  84,1,0,0},
    {7,3, NB_LAYOUT_CORE,     30,  86,1,0,0},
    {7,3, NB_LAYOUT_CORE,     32,  88,1,0,0},
    {7,3, NB_LAYOUT_FINAL,    35,  90,1,0,0},
    {5,3, NB_LAYOUT_FINAL,    40,  94,1,1,NB_BOSS_FINAL_HP}
};

static uint8_t s_fw = NB_W, s_fh = NB_H;
static nb_state_t s_state = NB_OFF;
static uint8_t s_level = 1;
static uint8_t s_selected_level = 1;
static uint8_t s_unlocked_level = 1;
static uint8_t s_lives = 3;
static uint8_t s_energy = 60;
static uint8_t s_combo = 0;
static uint32_t s_score = 0;
static uint32_t s_hi = 0;
static uint16_t s_bricks_left = 0;
static uint16_t s_level_breakable_total = 0;
static uint32_t s_back_t0 = 0;
static uint32_t s_last_update = 0;
static uint32_t s_last_render = 0;
static uint32_t s_state_t0 = 0;
static uint32_t s_level_t0 = 0;
static uint32_t s_combo_until = 0;
static uint32_t s_laser_next = 0;
static uint32_t s_cannon_charge = 0;
static uint32_t s_cannon_flash_until = 0;
static uint32_t s_effect_until = 0;
static uint32_t s_shield_until = 0;
static uint32_t s_magnet_until = 0;
static uint32_t s_slow_until = 0;
static uint32_t s_overdrive_until = 0;
static uint32_t s_wide_until = 0;
static uint8_t s_shield_charges = 2;
static uint8_t s_magnet_charges = 2;
static uint8_t s_slow_charges = 1;
static uint8_t s_multi_charges = 2;
static uint8_t s_overdrive_charges = 1;
static bool s_lt_held = false;
static bool s_rt_held = false;
static bool s_started = false;
static bool s_pad_lost = false;
static bool s_inited = false;
static uint32_t s_sim_accum = 0;
static int16_t s_paddle_x8 = 9 * 256;
static uint8_t s_previous_orient = MATRIX_ORIENT_LANDSCAPE;
static uint32_t s_rng = 0xB16B00B5u;
static bool s_boss_active = false;
static int16_t s_boss_x8 = 8 * 256;
static int16_t s_boss_vx8 = 64;
static uint8_t s_boss_hp = 0;
static uint32_t s_boss_next_drop = 0;

static nb_brick_t s_bricks[NB_MAX_BRICKS];
static nb_ball_t s_balls[NB_MAX_BALLS];
static nb_powerup_inst_t s_powerups[NB_MAX_POWERUPS];
static nb_shot_t s_shots[NB_MAX_SHOTS];
static nb_particle_t s_particles[NB_MAX_PARTICLES];
static nb_hazard_t s_hazards[4];

static void save_stats(void);
static uint8_t s_frame[NB_CELLS][3];
static uint32_t s_dirty[10];

/* -------------------------------------------------------------------------- */
/* Colors                                                                      */
/* -------------------------------------------------------------------------- */

static const rgb_t C_BG0      = {  2,  5, 12 };
static const rgb_t C_CYAN     = { 20,180,240 };
static const rgb_t C_BLUE     = { 35, 80,255 };
static const rgb_t C_GREEN    = { 20,220, 70 };
static const rgb_t C_YELLOW   = {250,200, 30 };
static const rgb_t C_ORANGE   = {255,110, 25 };
static const rgb_t C_RED      = {255, 30, 45 };
static const rgb_t C_MAGENTA  = {220, 35,170 };
static const rgb_t C_PURPLE   = {125, 45,255 };
static const rgb_t C_WHITE    = {160,230,255 };
static const rgb_t C_METAL    = { 45, 65, 90 };

static inline uint32_t nb_rand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static rgb_t scale_rgb(rgb_t c, uint8_t pct)
{
    rgb_t o;
    o.r = (uint8_t)((uint16_t)c.r * pct / 100u);
    o.g = (uint8_t)((uint16_t)c.g * pct / 100u);
    o.b = (uint8_t)((uint16_t)c.b * pct / 100u);
    return o;
}

static rgb_t brick_color(uint8_t row, uint8_t kind, uint8_t hp)
{
    static const rgb_t pal[7] = {
        { 40,220,255 }, { 55,100,255 }, { 80,255,110 },
        {255,220, 40 }, {255,130, 35 }, {255, 50, 65 },
        {210, 60,230 }
    };
    rgb_t c = pal[row % 7];
    switch (kind) {
        case BRICK_ARMOR:     c = scale_rgb(c, hp > 1 ? 88 : 68); break;
        case BRICK_EXPLOSIVE: c = C_ORANGE; break;
        case BRICK_POWER:     c = C_YELLOW; break;
        case BRICK_METAL:     c = C_METAL; break;
        default: break;
    }
    return c;
}

static void mark_dirty(int idx)
{
    if (idx < 0 || idx >= NB_CELLS) return;
    s_dirty[idx >> 5] |= 1u << (idx & 31);
}

static void px(int x, int y, rgb_t c)
{
    if (x < 0 || y < 0 || x >= s_fw || y >= s_fh) return;
    int idx = NB_IDX(x,y);
    s_frame[idx][0] = c.r;
    s_frame[idx][1] = c.g;
    s_frame[idx][2] = c.b;
    mark_dirty(idx);
    matrix_set_xy_rgb(x,y,c);
}

static void clear_frame(void)
{
    memset(s_frame, 0, sizeof(s_frame));
    memset(s_dirty, 0xFF, sizeof(s_dirty));
    matrix_clear();
}

static bool ball_alive_count(void)
{
    for (int i=0;i<NB_MAX_BALLS;i++) if (s_balls[i].active) return true;
    return false;
}

static int brick_index(uint8_t col, uint8_t row)
{
    if (col >= NB_BRICK_COLS || row >= NB_BRICK_ROWS) return -1;
    return (int)row * NB_BRICK_COLS + col;
}

static int paddle_width(uint32_t now)
{
    return (s_wide_until > now) ? 7 : 5;
}

static int paddle_cell_left(void)
{
    int x = s_paddle_x8 / 256;
    int w = (s_wide_until != 0) ? 7 : 5;
    if (x < 0) x = 0;
    if (x > s_fw - w) x = s_fw - w;
    return x;
}

static bool brick_at(int x, int y, int *out)
{
    for (int i=0;i<NB_MAX_BRICKS;i++) {
        nb_brick_t *b = &s_bricks[i];
        if (!b->alive) continue;
        int bx = NB_BRICK_X0 + b->col * 3;
        int by = NB_BRICK_Y0 + b->row;
        if (x >= bx && x < bx + NB_BRICK_W && y == by) {
            if (out) *out = i;
            return true;
        }
    }
    return false;
}

static void burst(int x, int y, uint8_t kind, uint8_t count)
{
    for (uint8_t n=0;n<count;n++) {
        int slot = -1;
        for (int i=0;i<NB_MAX_PARTICLES;i++) if (!s_particles[i].active) { slot=i; break; }
        if (slot < 0) return;
        nb_particle_t *p=&s_particles[slot];
        p->active=true; p->x=(int8_t)x; p->y=(int8_t)y;
        p->dx=(int8_t)((int)(nb_rand()%3)-1);
        p->dy=(int8_t)((int)(nb_rand()%3)-1);
        if (p->dx==0 && p->dy==0) p->dy=-1;
        p->life=(uint8_t)(4+(nb_rand()%5)); p->kind=kind;
    }
}

static void give_power(uint8_t type, uint32_t now)
{
    switch (type) {
        case PU_WIDE:      s_wide_until = now + 6000u; if (s_paddle_x8 > (s_fw-7)*256) s_paddle_x8=(s_fw-7)*256; break;
        case PU_SHIELD:    if (s_shield_charges < 3) s_shield_charges++; break;
        case PU_MAGNET:    if (s_magnet_charges < 3) s_magnet_charges++; break;
        case PU_SLOW:      if (s_slow_charges < 3) s_slow_charges++; break;
        case PU_MULTI:     if (s_multi_charges < 3) s_multi_charges++; break;
        case PU_ENERGY:    s_energy = (uint8_t)(s_energy > 80 ? 100 : s_energy + 20); break;
        case PU_BONUS:     s_score += 250; break;
        case PU_OVERDRIVE: if (s_overdrive_charges < 3) s_overdrive_charges++; break;
        default: break;
    }
}

static void spawn_powerup(int x, int y, uint8_t type)
{
    for (int i=0;i<NB_MAX_POWERUPS;i++) {
        if (!s_powerups[i].active) {
            s_powerups[i].active=true;
            s_powerups[i].x8=(int16_t)x*256;
            s_powerups[i].y8=(int16_t)y*256;
            s_powerups[i].type=type;
            return;
        }
    }
}

static void kill_ball(int bi, uint32_t now)
{
    if (!s_balls[bi].active) return;
    burst(s_balls[bi].x8/256, s_balls[bi].y8/256, 1, 5);
    s_balls[bi].active=false;
    if (!ball_alive_count()) {
        if (s_shield_until > now) {
            s_balls[bi].active=true;
            s_balls[bi].x8=s_paddle_x8*1 + 128;
            s_balls[bi].y8=(NB_PADDLE_Y-1)*256;
            s_balls[bi].vx8=0;
            s_balls[bi].vy8=-52;
            s_shield_until=0;
            return;
        }
        if (s_lives > 1) {
            s_lives--;
            s_combo=0;
            s_combo_until=0;
            for (int i=0;i<NB_MAX_BALLS;i++) s_balls[i].active=false;
            s_state=NB_READY;
            s_state_t0=now;
            s_energy = (uint8_t)(s_energy < 25 ? 25 : s_energy);
            snd_play("impact_hard");
        } else {
            s_lives=0;
            s_state=NB_OVER;
            s_state_t0=now;
            if (s_score > s_hi) { s_hi=s_score; snd_play("highscore_fanfare"); }
            else snd_play("game_over");
            save_stats();
        }
    }
}

static void reset_balls(bool serve)
{
    memset(s_balls,0,sizeof(s_balls));
    if (!serve) return;
    nb_ball_t *b=&s_balls[0];
    b->active=true;
    b->stuck=true;
    b->x8=(int16_t)(s_paddle_x8 + 128);
    b->y8=(NB_PADDLE_Y-1)*256;
    b->vx8=0;
    b->vy8=-(int16_t)s_levels[s_level-1].ball_speed;
}

static void reset_runtime(void)
{
    memset(s_powerups,0,sizeof(s_powerups));
    memset(s_shots,0,sizeof(s_shots));
    memset(s_particles,0,sizeof(s_particles));
    s_combo=0;
    s_combo_until=0;
    s_energy=60;
    s_cannon_charge=0;
    s_lt_held=false;
    s_rt_held=false;
    s_shield_until=0;
    s_magnet_until=0;
    s_slow_until=0;
    s_overdrive_until=0;
    s_wide_until=0;
    memset(s_hazards,0,sizeof(s_hazards));
    s_boss_active=false;
    s_boss_hp=0;
    s_boss_x8=8*256;
    s_boss_vx8=64;
    s_boss_next_drop=0;
}

static void put_brick(uint8_t col, uint8_t row, uint8_t kind, uint8_t hp)
{
    int idx=brick_index(col,row);
    if (idx<0) return;
    s_bricks[idx].alive=true;
    s_bricks[idx].col=col;
    s_bricks[idx].row=row;
    s_bricks[idx].kind=kind;
    s_bricks[idx].max_hp=hp;
    s_bricks[idx].hp=hp;
}

static bool layout_has(int c, int r, const nb_level_cfg_t *L)
{
    int cx=NB_BRICK_COLS/2;
    int cy=(int)L->rows/2;
    switch (L->layout) {
        case NB_LAYOUT_SOLID:   return r < L->rows;
        case NB_LAYOUT_STRIPES: return (r < L->rows) && ((r&1)==0 || c==0 || c==NB_BRICK_COLS-1);
        case NB_LAYOUT_CHECKER: return r < L->rows && (((c+r)&1)==0);
        case NB_LAYOUT_PYRAMID: return r < L->rows && (c >= r/2) && (c < NB_BRICK_COLS - r/2);
        case NB_LAYOUT_DIAMOND: {
            int dx=c-cx; if(dx<0) dx=-dx; int dy=r-cy; if(dy<0) dy=-dy;
            return r < L->rows && dx+dy <= 3;
        }
        case NB_LAYOUT_FORTRESS:
            return r < L->rows && (r==0 || r==(int)L->rows-1 || c==0 || c==NB_BRICK_COLS-1 || r==cy);
        case NB_LAYOUT_COLUMNS: return r < L->rows && (c==0 || c==2 || c==4 || c==6 || r==0 || r==(int)L->rows-1);
        case NB_LAYOUT_ZIGZAG:  return r < L->rows && (((c + r*2) % 4) != 1);
        case NB_LAYOUT_INVADER: return r < L->rows && (r==0 || r==1 || (r==2 && (c<2||c>4)) || (r>=3 && (c==1||c==5)));
        case NB_LAYOUT_CROSS:   return r < L->rows && (c==cx || r==cy || r==0 || r==(int)L->rows-1);
        case NB_LAYOUT_MINIBOSS: return r < L->rows && (r<2 || c==0 || c==6 || (r>=2 && r<=3 && (c==2||c==3||c==4)));
        case NB_LAYOUT_GATES:   return r < L->rows && (c==0 || c==3 || c==6 || r==0 || r==L->rows-1);
        case NB_LAYOUT_HOLE:    return r < L->rows && !((c==3||c==4) && r==2);
        case NB_LAYOUT_FRAME:   return r < L->rows && (r==0 || r==L->rows-1 || c==0 || c==6);
        case NB_LAYOUT_DENSE:   return r < L->rows && (((c+r)%3)!=1);
        case NB_LAYOUT_STAGGER: return r < L->rows && ((c + r) % 2 == 0 || r < 2);
        case NB_LAYOUT_CORE:    return r < L->rows && (r<2 || r==L->rows-1 || c<2 || c>4 || (r==3 && c>=2 && c<=4));
        case NB_LAYOUT_FINAL:   return r < L->rows && (r<2 || c==0 || c==6 || r==L->rows-1 || (r==3 && c>=2 && c<=4));
        default: return false;
    }
}

static void build_level(uint8_t level, uint32_t now, bool keep_score)
{
    if (level < 1) level=1;
    if (level > 20) level=20;
    const nb_level_cfg_t *L=&s_levels[level-1];
    s_level=level;
    memset(s_bricks,0,sizeof(s_bricks));
    reset_runtime();
    s_paddle_x8=9*256;
    s_bricks_left=0;
    s_level_breakable_total=0;

    for (uint8_t r=0;r<L->rows;r++) {
        for (uint8_t c=0;c<NB_BRICK_COLS;c++) {
            if (!layout_has(c,r,L)) continue;
            uint8_t kind=BRICK_NORMAL;
            uint8_t hp=L->hp_scale;
            if (level>=4 && ((c+r)%7)==0) kind=BRICK_POWER;
            if (level>=5 && ((c*2+r)%6)==0) kind=BRICK_EXPLOSIVE;
            if (level>=6 && ((c+r*3)%5)==0) { kind=BRICK_ARMOR; hp=(uint8_t)(L->hp_scale+1); }
            if (level>=13 && ((c*3+r)%11)==0) kind=BRICK_METAL;
            if (kind!=BRICK_METAL) { s_bricks_left++; s_level_breakable_total++; }
            put_brick(c,r,kind,hp);
        }
    }

    if (L->boss) {
        s_boss_active=true;
        s_boss_hp=L->boss_hp;
        /* Boss occupies no brick slots; its core sits above the brick field. */
        s_bricks_left=0;
        for (int i=0;i<NB_MAX_BRICKS;i++) if (s_bricks[i].alive && s_bricks[i].kind!=BRICK_METAL) s_bricks_left++;
    }

    reset_balls(true);
    s_level_t0=now;
    s_state_t0=now;
    s_effect_until=0;
    s_started=true;
    if (!keep_score) {
        s_score=0;
        s_lives=3;
        s_shield_charges=2;
        s_magnet_charges=2;
        s_slow_charges=1;
        s_multi_charges=2;
        s_overdrive_charges=1;
    } else {
        if (s_shield_charges==0) s_shield_charges=1;
    }
}

static void start_run(uint8_t level, uint32_t now)
{
    s_score=0;
    s_lives=3;
    s_shield_charges=2;
    s_magnet_charges=2;
    s_slow_charges=1;
    s_multi_charges=2;
    s_overdrive_charges=1;
    s_selected_level=level;
    build_level(level,now,false);
}

/* -------------------------------------------------------------------------- */
/* Persistence                                                                  */
/* -------------------------------------------------------------------------- */

static void load_stats(void)
{
    nvs_handle_t h;
    if (nvs_open("arcade", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t hi=0;
    uint8_t ul=1;
    if (nvs_get_u32(h,"breaker_hi",&hi)==ESP_OK) s_hi=hi;
    if (nvs_get_u8(h,"breaker_lvl",&ul)==ESP_OK && ul>=1 && ul<=20) s_unlocked_level=ul;
    nvs_close(h);
}

static void save_stats(void)
{
    nvs_handle_t h;
    if (nvs_open("arcade", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h,"breaker_hi",s_hi);
    nvs_set_u8(h,"breaker_lvl",s_unlocked_level);
    nvs_commit(h);
    nvs_close(h);
}

/* -------------------------------------------------------------------------- */
/* Gameplay                                                                    */
/* -------------------------------------------------------------------------- */

static void destroy_brick(int idx, uint32_t now, bool heavy)
{
    if (idx<0 || idx>=NB_MAX_BRICKS) return;
    nb_brick_t *b=&s_bricks[idx];
    if (!b->alive) return;
    int bx=NB_BRICK_X0+b->col*3;
    int by=NB_BRICK_Y0+b->row;

    if (b->kind==BRICK_METAL) {
        burst(bx,by,6,2);
        return;
    }

    if (b->hp > (heavy ? 1 : 0)) {
        b->hp--;
        burst(bx,by,2,2);
        s_score += 5;
        return;
    }

    b->alive=false;
    if (s_bricks_left) s_bricks_left--;
    uint16_t points=(uint16_t)(10u*(1u+b->row) + (uint16_t)(s_combo*3u));
    s_score += points;
    s_combo = (uint8_t)(s_combo < 9 ? s_combo + 1 : 9);
    s_combo_until=now+2500u;
    burst(bx,by,(uint8_t)b->kind, b->kind==BRICK_EXPLOSIVE?7:4);

    if (b->kind==BRICK_POWER) {
        uint8_t p=(uint8_t)(nb_rand()%PU_COUNT);
        spawn_powerup(bx,by,p);
    } else if ((nb_rand()%100u) < s_levels[s_level-1].special_rate) {
        uint8_t p=(uint8_t)(nb_rand()%PU_COUNT);
        spawn_powerup(bx,by,p);
    }

    if (b->kind==BRICK_EXPLOSIVE) {
        for (int j=0;j<NB_MAX_BRICKS;j++) {
            if (!s_bricks[j].alive || j==idx) continue;
            int dx=(int)s_bricks[j].col-(int)b->col;
            int dy=(int)s_bricks[j].row-(int)b->row;
            if (dx<0) dx=-dx;
            if (dy<0) dy=-dy;
            if (dx<=1 && dy<=1) destroy_brick(j,now,true);
        }
    }

    if (s_combo==3 || s_combo==6 || s_combo==9) {
        snd_play("score_tick");
    } else {
        snd_play("impact_light");
    }
}

static void hit_boss(uint32_t now, uint8_t damage)
{
    (void)now;
    if (!s_boss_active || s_boss_hp==0) return;
    s_boss_hp = s_boss_hp > damage ? (uint8_t)(s_boss_hp-damage) : 0;
    burst((s_boss_x8/256)+3,2,7,5);
    s_score += 35;
    if (s_boss_hp==0) {
        s_boss_active=false;
        s_score += 1000u*s_level;
        burst((s_boss_x8/256)+3,2,7,16);
        snd_play("highscore_fanfare");
    }
}

static void bounce_paddle(nb_ball_t *b, uint32_t now)
{
    int px0=paddle_cell_left();
    int w=paddle_width(now);
    int bx=b->x8/256;
    int rel=bx-px0;
    if (rel<0) rel=0;
    if (rel>w-1) rel=w-1;
    int16_t vx = (int16_t)(((rel*2 - (w-1)) * 150) / ((w-1)?(w-1):1));
    b->vx8=vx;
    int speed=s_levels[s_level-1].ball_speed;
    int vy=-(speed + (s_combo>5 ? 4 : 0));
    if (s_overdrive_until) vy=-((speed*125)/100);
    b->vy8=(int16_t)vy;
    b->y8=(NB_PADDLE_Y-1)*256;
}

static void update_paddle(const xbox360_pad_t *pad, uint32_t now)
{
    if (!pad) return;
    int ax=pad->lx;
    int step=1 + (ax>=0 ? ax : -ax)/9000;
    if (step>5) step=5;
    int x8=s_paddle_x8;
    if (ax > 8000) x8 += step*48;
    else if (ax < -8000) x8 -= step*48;
    if (x8 < 0) x8=0;
    int maxx=(s_fw-paddle_width(now))*256;
    if (x8 > maxx) x8=maxx;
    s_paddle_x8=(int16_t)x8;
}

static void spawn_extra_ball(int16_t x8, int16_t y8, int16_t vx8, int16_t vy8, bool fireball)
{
    for (int i=0;i<NB_MAX_BALLS;i++) {
        if (!s_balls[i].active) {
            s_balls[i].active=true;
            s_balls[i].stuck=false;
            s_balls[i].x8=x8;
            s_balls[i].y8=y8;
            s_balls[i].vx8=vx8;
            s_balls[i].vy8=vy8;
            s_balls[i].fireball=fireball;
            s_balls[i].power_hits=fireball?4:0;
            return;
        }
    }
}

static void launch_ball(uint32_t now)
{
    for (int i=0;i<NB_MAX_BALLS;i++) if (s_balls[i].active && s_balls[i].stuck) {
        s_balls[i].stuck=false;
        s_balls[i].vy8=-(int16_t)s_levels[s_level-1].ball_speed;
        s_balls[i].vx8 = 0;
        snd_play("ui_tap");
        s_state=NB_PLAY;
        s_state_t0=now;
        return;
    }
}

static void activate_shield(uint32_t now)
{
    if (s_shield_charges==0 || s_shield_until>now) return;
    s_shield_charges--;
    s_shield_until=now+2500u;
    burst(paddle_cell_left()+2,NB_PADDLE_Y,3,8);
    snd_play("impact_moderate");
}

static void activate_magnet(uint32_t now)
{
    if (s_magnet_charges==0 || s_magnet_until>now) return;
    s_magnet_charges--;
    s_magnet_until=now+4500u;
    snd_play("ui_toggle_on");
}

static void activate_slow(uint32_t now)
{
    if (s_slow_charges==0 || s_slow_until>now) return;
    s_slow_charges--;
    s_slow_until=now+3500u;
    snd_play("ui_toggle_on");
}

static void activate_multi(uint32_t now)
{
    (void)now;
    if (s_multi_charges==0 || !ball_alive_count()) return;
    s_multi_charges--;
    nb_ball_t base=s_balls[0];
    spawn_extra_ball(base.x8,base.y8,(int16_t)(base.vx8-70),base.vy8,true);
    spawn_extra_ball(base.x8,base.y8,(int16_t)(base.vx8+70),base.vy8,false);
    burst(base.x8/256,base.y8/256,4,8);
    snd_play("score_tick");
}

static void activate_overdrive(uint32_t now)
{
    if (s_overdrive_charges==0 || s_overdrive_until>now) return;
    s_overdrive_charges--;
    s_overdrive_until=now+3500u;
    snd_play("gear_shift_up");
}

static void add_cannon_shot(int16_t x8, uint32_t now)
{
    for (int i=0;i<NB_MAX_SHOTS;i++) {
        if (!s_shots[i].active) {
            s_shots[i].active=true;
            s_shots[i].x8=x8;
            s_shots[i].y8=(NB_PADDLE_Y-1)*256;
            s_shots[i].vy8=-170;
            s_shots[i].kind=SHOT_CANNON;
            s_shots[i].ttl=30;
            s_cannon_flash_until=now+180;
            return;
        }
    }
}

static void laser_tick(uint32_t now)
{
    if (!s_rt_held || s_energy < 2 || now < s_laser_next) return;
    s_laser_next=now+100u;
    int x=paddle_cell_left()+2;
    s_energy=(uint8_t)(s_energy-2);
    int closest=-1, besty=99;
    for (int i=0;i<NB_MAX_BRICKS;i++) {
        if (!s_bricks[i].alive || s_bricks[i].kind==BRICK_METAL) continue;
        int bx=NB_BRICK_X0+s_bricks[i].col*3;
        int by=NB_BRICK_Y0+s_bricks[i].row;
        if (x>=bx && x<bx+NB_BRICK_W && by<besty) { besty=by; closest=i; }
    }
    if (closest>=0) destroy_brick(closest,now,true);
    if (s_boss_active && x >= s_boss_x8/256 && x <= s_boss_x8/256+6) hit_boss(now,1);
}

static void cannon_tick(uint32_t now)
{
    if (!s_lt_held && s_cannon_charge) {
        uint32_t c=s_cannon_charge;
        s_cannon_charge=0;
        if (c >= 250u && s_energy>=10u) {
            s_energy=(uint8_t)(s_energy-10u);
            int16_t x8=(int16_t)((paddle_cell_left()+1)*256);
            add_cannon_shot(x8,now);
            add_cannon_shot((int16_t)(x8+2*256),now);
            snd_play("laser");
        }
    }
    if (s_lt_held && s_energy>0) {
        s_cannon_charge += 33u;
        if ((s_cannon_charge%132u)<33u) s_energy--;
        if (s_cannon_charge>1200u) s_cannon_charge=1200u;
    }
}

static void update_shots(uint32_t now)
{
    for (int i=0;i<NB_MAX_SHOTS;i++) if (s_shots[i].active) {
        nb_shot_t *q=&s_shots[i];
        q->y8 += q->vy8;
        if (q->ttl) q->ttl--; else q->active=false;
        int x=q->x8/256, y=q->y8/256;
        if (x<0||x>=s_fw||y<0) { q->active=false; continue; }
        if (s_boss_active && y<=3 && x>=s_boss_x8/256 && x<=s_boss_x8/256+6) {
            hit_boss(now,2);
            q->active=false; continue;
        }
        int idx=-1;
        if (brick_at(x,y,&idx)) {
            destroy_brick(idx,now,true);
            q->active=false;
        }
    }
}

static void maybe_level_clear(uint32_t now)
{
    if (s_boss_active || s_bricks_left != 0) return;
    s_score += (uint16_t)(100u*s_level);
    if (s_lives>0) s_score += (uint16_t)(50u*s_lives);
    s_effect_until=now+NB_LEVEL_FX_MS;
    s_state=NB_CLEAR;
    s_state_t0=now;
    snd_play("profile_chime_game");
}

static void update_powerups(uint32_t now)
{
    for (int i=0;i<NB_MAX_POWERUPS;i++) if (s_powerups[i].active) {
        nb_powerup_inst_t *p=&s_powerups[i];
        p->y8 += 34;
        int x=p->x8/256, y=p->y8/256;
        int px0=paddle_cell_left();
        if (y >= NB_PADDLE_Y) {
            if (x>=px0-1 && x<=px0+5) {
                give_power(p->type,now);
                if (p->type==PU_WIDE) s_magnet_until=now+5000u;
                burst(x,NB_PADDLE_Y,4,6);
                snd_play("score_tick");
            }
            p->active=false;
        }
    }
}

static void update_particles(void)
{
    for (int i=0;i<NB_MAX_PARTICLES;i++) if (s_particles[i].active) {
        nb_particle_t *p=&s_particles[i];
        p->x=(int8_t)(p->x+p->dx);
        p->y=(int8_t)(p->y+p->dy);
        if (p->life) p->life--; else p->active=false;
    }
}

static void update_boss(uint32_t now)
{
    if (!s_boss_active) return;
    s_boss_x8 += s_boss_vx8;
    if (s_boss_x8 < 0) { s_boss_x8=0; s_boss_vx8=-s_boss_vx8; }
    int maxx=(s_fw-7)*256;
    if (s_boss_x8 > maxx) { s_boss_x8=(int16_t)maxx; s_boss_vx8=-s_boss_vx8; }

    if (now >= s_boss_next_drop) {
        s_boss_next_drop=now+800u;
        int slot=-1;
        for (int i=0;i<4;i++) if (!s_hazards[i].active) { slot=i; break; }
        if (slot>=0) {
            s_hazards[slot].active=true;
            s_hazards[slot].x8=(int16_t)((s_boss_x8/256+3)*256);
            s_hazards[slot].y8=3*256;
            s_hazards[slot].vy8=52 + (int16_t)(s_level*2);
        }
    }
}

static void update_hazards(uint32_t now)
{
    int px0=paddle_cell_left();
    int pw=paddle_width(now);
    for (int i=0;i<4;i++) if (s_hazards[i].active) {
        nb_hazard_t *h=&s_hazards[i];
        h->y8 += h->vy8;
        int x=h->x8/256, y=h->y8/256;
        if (y>=NB_PADDLE_Y) {
            if (x>=px0-1 && x<=px0+pw) {
                s_hazards[i].active=false;
                if (s_shield_until>now) {
                    s_shield_until=0;
                    burst(x,NB_PADDLE_Y,3,7);
                } else {
                    s_lives = s_lives>1 ? (uint8_t)(s_lives-1) : 0;
                    s_combo=0;
                    if (s_lives==0) { s_state=NB_OVER; s_state_t0=now; snd_play("game_over"); }
                    else { s_state=NB_READY; s_state_t0=now; reset_balls(true); snd_play("impact_moderate"); }
                }
            } else {
                s_hazards[i].active=false;
            }
        }
    }
}

static void update_balls(uint32_t now, uint32_t dt)
{
    uint32_t scale=100;
    if (s_slow_until>now) scale=55;
    if (s_overdrive_until>now) scale=125;
    dt=(dt*scale)/100u;
    if (dt<4) dt=4;

    int paddle_l=paddle_cell_left();
    int paddle_r=paddle_l+paddle_width(now)-1;

    for (int i=0;i<NB_MAX_BALLS;i++) if (s_balls[i].active) {
        nb_ball_t *b=&s_balls[i];
        if (b->stuck) {
            b->x8=(int16_t)(s_paddle_x8+128);
            b->y8=(NB_PADDLE_Y-1)*256;
            continue;
        }

        b->x8 += (int16_t)((b->vx8*(int32_t)dt)/NB_SIM_MS);
        b->y8 += (int16_t)((b->vy8*(int32_t)dt)/NB_SIM_MS);
        int x=b->x8/256;
        int y=b->y8/256;

        if (x<=0) { b->x8=128; b->vx8=(int16_t)-b->vx8; burst(0,y,0,1); }
        if (x>=s_fw-1) { b->x8=(int16_t)((s_fw-1)*256-128); b->vx8=(int16_t)-b->vx8; burst(s_fw-1,y,0,1); }
        if (y<=0) { b->y8=128; b->vy8=(int16_t)-b->vy8; }

        if (s_boss_active && y<=3) {
            int bx=s_boss_x8/256;
            if (x>=bx && x<=bx+6) {
                hit_boss(now,b->fireball?2:1);
                b->vy8=(int16_t)-b->vy8;
                if (b->fireball && b->power_hits) b->power_hits--; 
            }
        }

        int bi=-1;
        if (y>=0 && brick_at(x,y,&bi)) {
            bool heavy=b->fireball;
            destroy_brick(bi,now,heavy);
            if (!heavy || b->power_hits==0) {
                b->vy8=(int16_t)-b->vy8;
            } else {
                b->power_hits--;
            }
        }

        if (b->vy8>0 && y>=NB_PADDLE_Y-1 && y<=NB_PADDLE_Y && x>=paddle_l-1 && x<=paddle_r+1) {
            bounce_paddle(b,now);
            burst(x,NB_PADDLE_Y-1,4,2);
            snd_play("impact_light");
            if (s_magnet_until>now) b->stuck=true;
        }

        if (s_magnet_until>now && b->stuck) {
            b->x8=(int16_t)(s_paddle_x8+128);
            b->y8=(NB_PADDLE_Y-1)*256;
        }

        if (y>=s_fh) kill_ball(i,now);
    }
}

static void simulation_step(uint32_t now)
{
    update_balls(now,NB_SIM_MS);
    update_shots(now);
    update_powerups(now);
    update_hazards(now);
    update_boss(now);
    update_particles();
    laser_tick(now);
    cannon_tick(now);
    if (s_combo && now>s_combo_until) s_combo=0;
    maybe_level_clear(now);
}

/* -------------------------------------------------------------------------- */
/* Rendering                                                                   */
/* -------------------------------------------------------------------------- */

static rgb_t power_color(uint8_t type)
{
    static const rgb_t c[PU_COUNT] = {
        { 60,255,120 }, { 70,210,255 }, {210, 60,255 }, { 80,140,255 },
        {255,220, 30 }, { 50,255,190 }, {255,150, 30 }, {255, 70, 40 }
    };
    return c[type%PU_COUNT];
}

static void render_hud(uint32_t now)
{
    /* Top row is a live status strip: lives, energy, combo and level progress. */
    for (int x=0;x<s_fw;x++) px(x,0,scale_rgb(C_BG0,70));

    /* lives: x 0..2 */
    for (int i=0;i<3;i++) px(i,0,i<s_lives?C_GREEN:C_RED);

    /* energy: x 4..10, compact 0..100 bar */
    int en=(s_energy*7)/100;
    for (int i=0;i<7;i++) px(4+i,0,i<en?C_CYAN:scale_rgb(C_BLUE,25));

    /* combo: x 12..15, high contrast. */
    for (int i=0;i<4;i++) px(12+i,0,i < (s_combo>4?4:s_combo)?C_YELLOW:scale_rgb(C_ORANGE,20));

    /* level: x 17..22, progress toward clear */
    int done=0;
    if (s_level_breakable_total>0) {
        done=(int)((uint32_t)(s_level_breakable_total-s_bricks_left)*6u/s_level_breakable_total);
    }
    if (s_boss_active) done=(int)((uint16_t)(6u*(s_levels[s_level-1].boss_hp-s_boss_hp))/(s_levels[s_level-1].boss_hp?s_levels[s_level-1].boss_hp:1));
    if (done<0) done=0;
    if (done>6) done=6;
    for (int i=0;i<6;i++) px(17+i,0,i<done?C_MAGENTA:scale_rgb(C_PURPLE,20));

    (void)now;
}

static void render_background(uint32_t now)
{
    for (int y=1;y<s_fh-1;y++) {
        px(0,y,scale_rgb(C_BLUE,22));
        px(s_fw-1,y,scale_rgb(C_PURPLE,18));
    }
    /* Sparse moving star field; never overpowering. */
    for (int i=0;i<14;i++) {
        int x=(int)((i*11 + now/250u)%((uint32_t)(s_fw-2)))+1;
        int y=7 + (int)((i*5 + now/400u)%5u);
        px(x,y,scale_rgb(i&1?C_CYAN:C_PURPLE,(uint8_t)(8+(i%3)*4)));
    }
}

static void render_bricks(uint32_t now)
{
    for (int i=0;i<NB_MAX_BRICKS;i++) if (s_bricks[i].alive) {
        nb_brick_t *b=&s_bricks[i];
        int x=NB_BRICK_X0+b->col*3;
        int y=NB_BRICK_Y0+b->row;
        rgb_t c=brick_color(b->row,b->kind,b->hp);
        uint8_t pulse=(uint8_t)(84u + (uint8_t)((now/90u + i*7u)%42u));
        c=scale_rgb(c,pulse);
        px(x,y,c); px(x+1,y,scale_rgb(c,72));
    }
}

static void render_boss(uint32_t now)
{
    if (!s_boss_active) return;
    int bx=s_boss_x8/256;
    bool pulse=((now/120u)&1u)==0;
    rgb_t core=pulse?C_RED:C_MAGENTA;
    for (int x=0;x<7;x++) px(bx+x,2,scale_rgb(core,82));
    for (int x=1;x<6;x++) px(bx+x,3,scale_rgb(C_PURPLE,56));
    px(bx+3,2, pulse?C_WHITE:C_YELLOW);
    /* HP indicator on boss body */
    int hp=(s_boss_hp*7u)/(s_levels[s_level-1].boss_hp?s_levels[s_level-1].boss_hp:1u);
    if (hp<0) hp=0;
    if (hp>7) hp=7;
    for (int i=0;i<7;i++) px(bx+i,1,i<hp?C_RED:scale_rgb(C_RED,18));
}

static void render_hazards(void)
{
    for (int i=0;i<4;i++) if (s_hazards[i].active) {
        int x=s_hazards[i].x8/256;
        int y=s_hazards[i].y8/256;
        px(x,y,C_RED);
        if (y>0) px(x,y-1,scale_rgb(C_ORANGE,40));
    }
}

static void render_powerups(uint32_t now)
{
    bool blink=((now/120u)&1u)==0;
    for (int i=0;i<NB_MAX_POWERUPS;i++) if (s_powerups[i].active && blink) {
        int x=s_powerups[i].x8/256;
        int y=s_powerups[i].y8/256;
        rgb_t c=power_color(s_powerups[i].type);
        px(x,y,c);
        if ((i&1)==0 && x+1<s_fw) px(x+1,y,scale_rgb(c,45));
    }
}

static void render_shots(uint32_t now)
{
    for (int i=0;i<NB_MAX_SHOTS;i++) if (s_shots[i].active) {
        int x=s_shots[i].x8/256;
        int y=s_shots[i].y8/256;
        px(x,y,s_shots[i].kind==SHOT_LASER?C_WHITE:C_YELLOW);
        if (y+1<s_fh) px(x,y+1,scale_rgb(C_ORANGE,35));
    }
    if (s_rt_held) {
        int x=paddle_cell_left()+2;
        for (int y=4;y<NB_PADDLE_Y;y++) px(x,y,scale_rgb(C_CYAN,42));
    }
    if (s_cannon_flash_until>now) {
        int x=paddle_cell_left()+1;
        px(x,NB_PADDLE_Y-1,C_WHITE);
        if (x+1<s_fw) px(x+1,NB_PADDLE_Y-1,C_WHITE);
    }
}

static void render_paddle(uint32_t now)
{
    int x=paddle_cell_left();
    int w=paddle_width(now);
    rgb_t p=s_magnet_until>now?C_MAGENTA:C_CYAN;
    if (s_overdrive_until>now) p=C_ORANGE;
    for (int i=0;i<w;i++) px(x+i,NB_PADDLE_Y,p);
    if (s_shield_until>now) {
        int w=paddle_width(now)+2;
        for (int i=0;i<w;i++) {
            int sx=x-1+i;
            if (sx>=0 && sx<s_fw) px(sx,NB_PADDLE_Y-1,scale_rgb(C_BLUE,75));
        }
    }
}

static void render_balls(uint32_t now)
{
    for (int i=0;i<NB_MAX_BALLS;i++) if (s_balls[i].active) {
        int x=s_balls[i].x8/256;
        int y=s_balls[i].y8/256;
        rgb_t c=s_balls[i].fireball?C_ORANGE:C_WHITE;
        if (s_balls[i].stuck) c=C_YELLOW;
        px(x,y,c);
        if (s_balls[i].fireball && y+1<s_fh) px(x,y+1,scale_rgb(C_RED,40));
        (void)now;
    }
}

static void render_particles(void)
{
    for (int i=0;i<NB_MAX_PARTICLES;i++) if (s_particles[i].active) {
        nb_particle_t *p=&s_particles[i];
        rgb_t c;
        switch (p->kind & 7u) {
            case 0: c=C_CYAN; break;
            case 1: c=C_RED; break;
            case 2: c=C_YELLOW; break;
            case 3: c=C_BLUE; break;
            case 4: c=C_MAGENTA; break;
            case 5: c=C_GREEN; break;
            case 6: c=C_METAL; break;
            default: c=C_WHITE; break;
        }
        px(p->x,p->y,scale_rgb(c,(uint8_t)(30u+p->life*9u)));
    }
}

static void render_play(uint32_t now)
{
    clear_frame();
    render_background(now);
    render_hud(now);
    render_bricks(now);
    render_boss(now);
    render_hazards();
    render_powerups(now);
    render_shots(now);
    render_balls(now);
    render_paddle(now);
    render_particles();
    matrix_show();
}

static void render_menu(uint32_t now)
{
    clear_frame();
    /* Colorful attract mode: top brick preview + paddle + ball. */
    const nb_level_cfg_t *L=&s_levels[s_selected_level-1];
    for (int r=0;r<3;r++) for (int c=0;c<7;c++) {
        if (!layout_has(c,r,L)) continue;
        int x=NB_BRICK_X0+c*3, y=2+r;
        rgb_t col=brick_color((uint8_t)r,BRICK_NORMAL,1);
        px(x,y,col); px(x+1,y,scale_rgb(col,72));
    }
    int px0=9;
    for (int i=0;i<5;i++) px(px0+i,11,C_CYAN);
    int bx=(int)((now/180u)%17u)+3;
    int by=7 + (int)(((now/330u)%3u));
    px(bx,by,C_WHITE);
    for (int i=0;i<s_lives;i++) px(1+i,12,C_GREEN);
    /* selection beacon on level row */
    int beacon=(int)(s_selected_level%6)+16;
    px(beacon,12, C_YELLOW);
    matrix_show();
}

static void render_ready(uint32_t now)
{
    clear_frame();
    render_hud(now);
    int x=paddle_cell_left();
    for (int i=0;i<5;i++) px(x+i,NB_PADDLE_Y,C_CYAN);
    int b=(int)((now/200u)%2u);
    px(x+2,NB_PADDLE_Y-1,b?C_YELLOW:C_WHITE);
    for (int i=0;i<3;i++) px(9+i,6,(i==(int)((now/450u)%3u))?C_GREEN:scale_rgb(C_GREEN,25));
    matrix_show();
}

static void render_clear(uint32_t now)
{
    clear_frame();
    uint32_t el=now-s_state_t0;
    for (int x=0;x<s_fw;x++) {
        uint8_t p=(uint8_t)(35u+((x+(int)(el/60u))*12u)%65u);
        px(x,0,scale_rgb(C_YELLOW,p));
        px(x,s_fh-1,scale_rgb(C_CYAN,(uint8_t)(100-p/2)));
    }
    for (int y=1;y<s_fh-1;y++) {
        int x=(int)((el/50u + y*2u)%s_fw);
        px(x,y,C_WHITE);
    }
    matrix_show();
}

static void render_over(uint32_t now)
{
    clear_frame();
    uint32_t el=now-s_state_t0;
    bool flash=((el/180u)&1u)==0;
    rgb_t c=flash?C_RED:scale_rgb(C_MAGENTA,35);
    for (int x=0;x<s_fw;x++) px(x,0,c);
    for (int y=0;y<s_fh;y++) { px(0,y,c); px(s_fw-1,y,c); }
    /* compact score meter */
    int meter=(int)((s_score%1000u)*21u/1000u);
    for (int x=1;x<=meter;x++) px(x,10,flash?C_YELLOW:C_ORANGE);
    for (int i=0;i<s_lives;i++) px(4+i,11,C_GREEN);
    matrix_show();
}

/* -------------------------------------------------------------------------- */
/* State / routing                                                             */
/* -------------------------------------------------------------------------- */

void neon_breaker_init(void)
{
    if (s_inited) return;
    s_inited=true;
    s_fw=matrix_field_w();
    s_fh=matrix_field_h();
    if (s_fw!=NB_W || s_fh!=NB_H) {
        /* Breaker is designed for the 23x13 board. Keep rendering safe if
           another orientation is selected: field dimensions come from driver. */
        ESP_LOGW(TAG,"field is %ux%u; expected 23x13",s_fw,s_fh);
    }
    s_state=NB_OFF;
    s_hi=0;
    s_unlocked_level=1;
    load_stats();
}

bool neon_breaker_open(void)
{
    neon_breaker_init();
    if (!led_matrix_ready() || g.estop) return false;
    if (!os_parked()) return false;
    if (!led_matrix_acquire(OWNER_ARCADE)) return false;
    s_previous_orient = matrix_get_orient();
    matrix_set_orient(MATRIX_ORIENT_LANDSCAPE);
    s_fw = matrix_field_w();
    s_fh = matrix_field_h();
    led_matrix_set_demo(false);
    led_matrix_set_dim(false);
    matrix_set_dim_pct(100);
    input_flush_context_switch();
    s_state=NB_MENU;
    s_state_t0=0;
    s_back_t0=0;
    s_selected_level=(s_selected_level<1||s_selected_level>s_unlocked_level)?1:s_selected_level;
    snd_play("ui_open_sheet");
    notif_push(NOTIF_RULES,"NEON BREAKER: CORE WAR",0);
    return true;
}

bool neon_breaker_active(void)
{
    return s_state!=NB_OFF;
}

uint8_t neon_breaker_state(void)
{
    return (uint8_t)s_state;
}

bool neon_breaker_suppress_oled(void)
{
    return s_state==NB_READY || s_state==NB_PLAY || s_state==NB_PAUSED || s_state==NB_CLEAR;
}

static void exit_full(uint32_t now)
{
    s_state=NB_OFF;
    s_back_t0=0;
    s_pad_lost=false;
    if (s_score > s_hi) s_hi=s_score;
    save_stats();
    input_consume_all();
    matrix_set_dim_pct(100);
    led_matrix_safe_off();
    matrix_set_orient(s_previous_orient);
    s_fw = matrix_field_w();
    s_fh = matrix_field_h();
    led_matrix_release(OWNER_ARCADE);
    led_matrix_set_demo(true);
    snd_play("ui_back");
    os_ctx_t *ctx = car_os_ctx();
    if (ctx && ctx->state == OS_GAME_5) os_request_screen(ctx, OS_GAMES_HUB, now);
}

bool neon_breaker_route(const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now)
{
    neon_breaker_init();
    if (s_state==NB_OFF) return false;

    /* Let the car's global safety handler consume GUIDE and latch E-stop. */
    if (tap & B_GUIDE) return false;

    /* D-pad bits are deliberately ignored. */
    if (dig & B_BACK) {
        if (!s_back_t0) s_back_t0=now;
        if (now-s_back_t0>=NB_BACK_HOLD_MS) { exit_full(now); return false; }
    } else {
        s_back_t0=0;
    }

    if (g.estop) { exit_full(now); return false; }

    if (!pad || !pad->present) {
        if (s_state==NB_PLAY || s_state==NB_READY) {
            s_state=NB_PAUSED;
            s_state_t0=now;
            s_pad_lost=true;
            led_matrix_set_dim(true);
        }
        return true;
    }
    if (s_pad_lost) {
        s_pad_lost=false;
        led_matrix_set_dim(false);
        input_flush_context_switch();
        return true;
    }

    update_paddle(pad,now);

    if ((tap & B_BACK) && s_state != NB_MENU) {
        s_state=NB_MENU;
        s_state_t0=now;
        s_rt_held=false;
        s_lt_held=false;
        s_sim_accum=0;
        snd_play("ui_back");
        return true;
    }

    switch (s_state) {
        case NB_MENU:
            /* LB/RB = level selection. START = launch. D-pad unused. */
            if (tap & B_LB) { if (s_selected_level>1) s_selected_level--; snd_play("ui_nav"); }
            if (tap & B_RB) { if (s_selected_level<s_unlocked_level) s_selected_level++; snd_play("ui_nav"); }
            if (tap & B_START) {
                start_run(s_selected_level,now);
                s_state=NB_READY;
                s_state_t0=now;
                snd_play("profile_chime_game");
            }
            break;

        case NB_READY:
            if (tap & B_A) launch_ball(now);
            if (tap & B_START) launch_ball(now);
            break;

        case NB_PLAY:
            if (tap & B_B) activate_shield(now);
            if (tap & B_X) activate_magnet(now);
            if (tap & B_Y) activate_multi(now);
            if (tap & B_LB) activate_slow(now);
            if (tap & B_RB) activate_overdrive(now);

            s_rt_held=(pad->rt>35);
            s_lt_held=(pad->lt>25);

            if (tap & B_A) {
                /* A performs a quick paddle pulse when the ball is in play:
                   re-center a stuck/magnet ball and immediately release it. */
                for (int i=0;i<NB_MAX_BALLS;i++) if (s_balls[i].active && s_balls[i].stuck) {
                    s_balls[i].stuck=false;
                    s_balls[i].vx8=0;
                    s_balls[i].vy8=-(int16_t)s_levels[s_level-1].ball_speed;
                }
            }
            if (tap & B_START) {
                s_state=NB_PAUSED;
                s_state_t0=now;
                s_rt_held=false;
                s_lt_held=false;
                matrix_set_dim_pct(35);
                snd_play("ui_open_sheet");
            }
            break;

        case NB_PAUSED:
            if (tap & B_START) {
                s_state=NB_PLAY;
                s_state_t0=now;
                s_last_update=now;
                matrix_set_dim_pct(100);
                snd_play("ui_close_sheet");
            }
            break;

        case NB_CLEAR:
            if (tap & B_START) {
                uint8_t next=(uint8_t)(s_level<20?s_level+1:20);
                if (next>s_unlocked_level && s_level<20) { s_unlocked_level=next; save_stats(); }
                build_level(next,now,true);
                s_state=NB_READY;
                s_state_t0=now;
            }
            if (tap & B_BACK) { s_state=NB_MENU; s_state_t0=now; }
            break;

        case NB_OVER:
            if (tap & B_START && now-s_state_t0>700u) {
                start_run(s_selected_level,now);
                s_state=NB_READY;
                s_state_t0=now;
                snd_play("ui_tap");
            }
            if (tap & B_BACK) { s_state=NB_MENU; s_state_t0=now; }
            break;
        default:
            break;
    }

    return true;
}

void neon_breaker_tick(uint32_t now)
{
    if (s_state==NB_OFF) return;
    if (s_state==NB_MENU) {
        if (now-s_last_render>=NB_FRAME_MS) { s_last_render=now; render_menu(now); }
        return;
    }
    if (s_state==NB_READY) {
        /* Safety: keep balls attached to paddle until A/START launches. */
        if (now-s_last_render>=NB_FRAME_MS) { s_last_render=now; render_ready(now); }
        return;
    }
    if (s_state==NB_PLAY) {
        if (!s_last_update) s_last_update=now;
        uint32_t delta=now-s_last_update;
        if (delta>100) delta=100;
        s_last_update=now;
        s_sim_accum += delta;
        while (s_sim_accum>=NB_SIM_MS && s_state==NB_PLAY) {
            s_sim_accum-=NB_SIM_MS;
            simulation_step(now);
        }
        if (now-s_last_render>=NB_FRAME_MS) { s_last_render=now; render_play(now); }
        return;
    }
    if (s_state==NB_PAUSED) {
        if (now-s_last_render>=100) { s_last_render=now; render_play(now); }
        return;
    }
    if (s_state==NB_CLEAR) {
        if (now-s_last_render>=NB_FRAME_MS) { s_last_render=now; render_clear(now); }
        if (now-s_state_t0>=NB_LEVEL_FX_MS && s_level<20) {
            uint8_t next=(uint8_t)(s_level+1);
            if (next>s_unlocked_level) { s_unlocked_level=next; save_stats(); }
            build_level(next,now,true);
            s_state=NB_READY;
            s_state_t0=now;
        } else if (now-s_state_t0>=NB_LEVEL_FX_MS && s_level>=20) {
            s_state=NB_OVER;
            s_state_t0=now;
        }
        return;
    }
    if (s_state==NB_OVER) {
        if (now-s_last_render>=100) { s_last_render=now; render_over(now); }
    }
}

void neon_breaker_mx_init(void) { neon_breaker_init(); }
void neon_breaker_mx_update(const xbox360_pad_t *pad, uint32_t now_ms)
{
    (void)pad; (void)now_ms;
}
void neon_breaker_mx_draw(uint32_t now_ms) { (void)now_ms; }

void neon_breaker_get_status(neon_breaker_status_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out));
    out->state=(uint8_t)s_state;
    out->level=s_level;
    out->lives=s_lives;
    out->shield_charges=s_shield_charges;
    out->magnet_charges=s_magnet_charges;
    out->slow_charges=s_slow_charges;
    out->multi_charges=s_multi_charges;
    out->overdrive_charges=s_overdrive_charges;
    out->energy=s_energy;
    out->combo=s_combo;
    out->bricks_left=s_bricks_left;
    out->score=s_score;
    out->hi=s_hi;
    out->boss_active=s_boss_active?1:0;
    out->boss_hp=s_boss_hp;
    out->fw=s_fw;
    out->fh=s_fh;
    memcpy(out->frame_rgb,s_frame,sizeof(out->frame_rgb));
    memcpy(out->mirror_dirty,s_dirty,sizeof(out->mirror_dirty));
    memset(s_dirty,0,sizeof(s_dirty));
}
