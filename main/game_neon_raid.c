/*
 * NEON RAID: CORE BREACH
 * 299-LED / 23x13 matrix arcade game for the ESP32-S3 Car OS.
 *
 * Controller:
 *   Left stick  = move
 *   Right stick = aim
 *   A           = fire current weapon
 *   B           = dash
 *   X           = shield
 *   Y           = EMP
 *   LB          = switch weapon
 *   RB          = laser ability
 *   LT          = charge shot
 *   RT          = rapid fire
 *   START       = pause/resume/start/retry
 *   BACK        = menu/exit
 *   D-pad       = COMPLETELY IGNORED
 *
 * Vehicle motors are never touched by this module.
 */
#include "game_neon_raid.h"
#include "led_matrix.h"
#include "car_global.h"
#include "car_os.h"
#include "input_events.h"
#include "snd_bank.h"
#include "notif.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "neon_raid";

#define NR_W_MAX            23
#define NR_H_MAX            23
#define NR_CELLS_MAX        299
#define NR_MAX_ENEMIES      12
#define NR_MAX_BULLETS      28
#define NR_MAX_PARTICLES    28
#define NR_MAX_PICKUPS       8
#define NR_MAX_GENERATORS    3
#define NR_BACK_HOLD_MS   1000u
#define NR_FRAME_MS         33u
#define NR_MOVE_MS         115u
#define NR_BULLET_MS        55u
#define NR_ENEMY_AI_MS      180u
#define NR_HIT_IFRAME_MS    650u
#define NR_DASH_MS          900u
#define NR_SHIELD_MS       2200u
#define NR_SHIELD_CD_MS    5200u
#define NR_EMP_CD_MS       3800u
#define NR_LASER_CD_MS     1900u
#define NR_COMBO_WINDOW_MS 2600u
#define NR_SURVIVE_MS     26000u

#define NR_IDX(x,y) ((int)((y) * s_fw + (x)))

typedef struct { int8_t x, y; } nr_cell_t;
typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } nr_dir_t;
typedef enum { NR_OFF, NR_MENU, NR_COUNTDOWN, NR_PLAY, NR_PAUSED, NR_CLEAR, NR_OVER } nr_state_t;
typedef enum { OBJ_CLEAR = 0, OBJ_COLLECT, OBJ_DEFEND, OBJ_REACH, OBJ_GENERATORS, OBJ_SURVIVE, OBJ_BOSS } nr_objective_t;
typedef enum { EN_DRONE = 0, EN_HUNTER, EN_SHOOTER, EN_FAST, EN_SHIELD, EN_BOSS } nr_enemy_type_t;
typedef enum { W_PULSE = 0, W_CANNON, W_SCATTER, W_COUNT } nr_weapon_t;
typedef enum { PICK_ENERGY = 0, PICK_SCORE, PICK_SHIELD } nr_pick_type_t;

typedef struct {
    const char *name;
    nr_objective_t obj;
    uint8_t obj_target;
    uint8_t enemy_count;
    uint8_t hunter_count;
    uint8_t shooter_count;
    uint8_t fast_count;
    uint8_t shield_count;
    uint8_t obstacle_style;
    uint8_t boss;
    uint16_t boss_hp;
    uint16_t enemy_step_ms;
    uint8_t score_mult;
    uint8_t core_hp;
} nr_level_cfg_t;

typedef struct {
    bool active;
    nr_enemy_type_t type;
    nr_cell_t p;
    nr_dir_t dir;
    uint8_t hp;
    uint8_t shield_dir;
    uint32_t next_move;
    uint32_t next_fire;
    uint8_t phase;
} nr_enemy_t;

typedef struct {
    bool active;
    bool from_player;
    nr_cell_t p;
    nr_dir_t dir;
    uint8_t damage;
    uint8_t ttl;
    uint32_t next_step;
} nr_bullet_t;

typedef struct {
    bool active;
    nr_cell_t p;
    uint8_t type;
    uint32_t until;
} nr_pickup_t;

typedef struct {
    bool active;
    nr_cell_t p;
    uint8_t hp;
} nr_generator_t;

typedef struct {
    bool active;
    nr_cell_t p;
    uint8_t life;
    uint8_t color_kind;
} nr_particle_t;

typedef struct {
    nr_cell_t p;
    nr_dir_t move_dir;
    nr_dir_t aim_dir;
    uint8_t hp;
    uint8_t max_hp;
    uint8_t energy;
    nr_weapon_t weapon;
    uint32_t next_move;
    uint32_t next_fire;
    uint32_t next_dash;
    uint32_t shield_until;
    uint32_t shield_cd_until;
    uint32_t emp_cd_until;
    uint32_t laser_cd_until;
    uint32_t hit_iframe_until;
    uint16_t charge_ms;
    bool lt_was_held;
    uint16_t kills;
    uint8_t combo;
    uint32_t combo_until;
} nr_player_t;

typedef struct {
    nr_state_t state;
    uint8_t level;
    uint8_t lives;
    uint32_t score;
    uint32_t hi;
    uint8_t objective_now;
    uint16_t elapsed_ms;
    uint16_t clear_t0;
    uint16_t over_t0;
    uint8_t core_hp;
    bool gate_open;
} nr_game_t;

static const nr_level_cfg_t s_levels[20] = {
    {"FIRST CONTACT", OBJ_CLEAR,      3,  3,0,0,0,0, 0,0,0, 520,1,100},
    {"ENERGY RUN",    OBJ_COLLECT,    3,  4,0,0,0,0, 1,0,0, 500,1,100},
    {"HUNTER",        OBJ_CLEAR,      5,  4,1,0,0,0, 2,0,0, 480,1,100},
    {"GRID TRAP",     OBJ_REACH,      4,  5,1,0,0,0, 3,0,0, 470,1,100},
    {"SHIELD GUARD",  OBJ_CLEAR,      7,  5,0,1,0,1, 4,0,0, 455,1,100},
    {"CORE DEFENSE",  OBJ_DEFEND,    26000/1000, 6,1,1,0,0, 5,0,0, 445,1,100},
    {"FAST RAID",     OBJ_CLEAR,      8,  5,1,0,2,0, 1,0,0, 430,2,100},
    {"SPLIT LANE",    OBJ_CLEAR,     10,  5,2,1,1,0, 6,0,0, 420,2,100},
    {"FOUR GATES",    OBJ_GENERATORS, 3,  6,1,1,1,0, 7,0,0, 410,2,100},
    {"MINI BOSS",     OBJ_BOSS,       1,  0,0,0,0,0, 8,1,12,390,2,100},
    {"AMBUSH",        OBJ_CLEAR,     12,  7,2,1,2,0, 2,0,0, 380,2,100},
    {"LOW POWER",     OBJ_COLLECT,    5,  8,1,1,1,1, 9,0,0, 370,2,100},
    {"CROSS FIRE",    OBJ_CLEAR,     13,  6,2,3,0,0, 10,0,0,360,3,100},
    {"GENERATOR WAR", OBJ_GENERATORS, 3,  9,2,2,1,0, 11,0,0,350,3,100},
    {"HUNTER PACK",   OBJ_CLEAR,     15,  7,4,1,0,0, 12,0,0,340,3,100},
    {"SURVIVE",       OBJ_SURVIVE,   24,  9,3,2,2,1, 13,0,0,330,3,100},
    {"BLACKOUT",      OBJ_CLEAR,     17,  8,3,3,2,1, 14,0,0,320,4,100},
    {"MULTI HAZARD",  OBJ_CLEAR,     19,  9,3,3,2,1, 15,0,0,310,4,100},
    {"SURVIVAL",      OBJ_SURVIVE,   38, 10,3,3,3,1, 16,0,0,300,4,100},
    {"CORE OVERLOAD", OBJ_BOSS,       1,  0,0,0,0,0, 17,1,35,285,5,100},
};

static uint8_t s_fw = 23, s_fh = 13;
static nr_state_t s_state = NR_OFF;
static nr_game_t s_game;
static nr_player_t s_player;
static nr_enemy_t s_enemies[NR_MAX_ENEMIES];
static nr_bullet_t s_bullets[NR_MAX_BULLETS];
static nr_pickup_t s_pickups[NR_MAX_PICKUPS];
static nr_generator_t s_generators[NR_MAX_GENERATORS];
static nr_particle_t s_particles[NR_MAX_PARTICLES];
static uint8_t s_block[NR_CELLS_MAX];
static uint32_t s_back_t0 = 0;
static uint32_t s_last_render = 0;
static uint32_t s_state_t0 = 0;
static uint32_t s_countdown_step = 99;
static uint32_t s_last_ai = 0;
static uint32_t s_level_t0 = 0;
static uint32_t s_hi = 0;
static uint8_t s_selected_level = 1;
static uint8_t s_unlocked = 1;
static bool s_inited = false;
static bool s_pad_lost = false;
static uint8_t s_last_event = 0;
static uint8_t s_frame[NR_CELLS_MAX][3];
static uint32_t s_dirty[10];
static uint32_t s_rng = 0x71C0FFEEu;
static bool s_old_start = false;

static inline uint32_t nr_rand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static inline bool in_bounds(int x, int y)
{
    return x >= 0 && y >= 0 && x < (int)s_fw && y < (int)s_fh;
}

static inline void mark_dirty(int idx)
{
    if (idx >= 0 && idx < NR_CELLS_MAX) s_dirty[idx >> 5] |= 1u << (idx & 31);
}

static void px(int x, int y, rgb_t c)
{
    if (!in_bounds(x,y)) return;
    int idx = NR_IDX(x,y);
    s_frame[idx][0] = c.r;
    s_frame[idx][1] = c.g;
    s_frame[idx][2] = c.b;
    mark_dirty(idx);
    matrix_set_xy_rgb(x,y,c);
}

static void clear_frame(void)
{
    for (int i=0;i<NR_CELLS_MAX;i++) {
        s_frame[i][0] = s_frame[i][1] = s_frame[i][2] = 0;
        mark_dirty(i);
    }
    matrix_clear();
}

static rgb_t scale_rgb(rgb_t c, uint8_t pct)
{
    rgb_t o;
    o.r = (uint8_t)((uint16_t)c.r * pct / 100u);
    o.g = (uint8_t)((uint16_t)c.g * pct / 100u);
    o.b = (uint8_t)((uint16_t)c.b * pct / 100u);
    return o;
}

static nr_dir_t dir_from_stick(int16_t x, int16_t y, nr_dir_t fallback, uint16_t th)
{
    int ax = x >= 0 ? x : -x;
    int ay = y >= 0 ? y : -y;
    if (ax < th && ay < th) return fallback;
    if (ax >= ay) return x >= 0 ? DIR_RIGHT : DIR_LEFT;
    return y >= 0 ? DIR_UP : DIR_DOWN;
}

static void step_cell(nr_cell_t *p, nr_dir_t d)
{
    switch (d) {
    case DIR_UP:    p->y++; break;
    case DIR_DOWN:  p->y--; break;
    case DIR_LEFT:  p->x--; break;
    case DIR_RIGHT: p->x++; break;
    }
}

static nr_cell_t ahead(nr_cell_t p, nr_dir_t d)
{
    step_cell(&p,d); return p;
}

static bool cell_blocked(int x, int y)
{
    if (!in_bounds(x,y)) return true;
    return s_block[NR_IDX(x,y)] != 0;
}

static bool occupied_by_enemy(int x, int y, int skip)
{
    for (int i=0;i<NR_MAX_ENEMIES;i++) {
        if (!s_enemies[i].active || i == skip) continue;
        if (s_enemies[i].p.x == x && s_enemies[i].p.y == y) return true;
    }
    return false;
}

static bool occupied_by_pickup(int x,int y)
{
    for (int i=0;i<NR_MAX_PICKUPS;i++)
        if (s_pickups[i].active && s_pickups[i].p.x == x && s_pickups[i].p.y == y) return true;
    return false;
}

static bool occupied_by_generator(int x,int y)
{
    for (int i=0;i<NR_MAX_GENERATORS;i++)
        if (s_generators[i].active && s_generators[i].p.x == x && s_generators[i].p.y == y) return true;
    return false;
}

static void build_level_geometry(uint8_t style)
{
    memset(s_block, 0, sizeof(s_block));

    if (style == 0) return;
    int w = s_fw, h = s_fh;
    int cx = w/2, cy = h/2;

    if (style == 1) {
        for (int y=2;y<h-2;y++) { if (y!=cy) { s_block[NR_IDX(5,y)]=1; s_block[NR_IDX(w-6,y)]=1; } }
    } else if (style == 2) {
        for (int x=3;x<w-3;x++) { if (x!=cx) { s_block[NR_IDX(x,3)]=1; s_block[NR_IDX(x,h-4)]=1; } }
    } else if (style == 3) {
        for (int x=2;x<w-2;x++) if (x<cx-1||x>cx+1) s_block[NR_IDX(x,cy)]=1;
        for (int y=2;y<h-2;y++) if (y<cy-1||y>cy+1) s_block[NR_IDX(cx,y)]=1;
    } else {
        int gap = (style % 3) + 1;
        int bars = 2 + (style % 3);
        for (int b=0;b<bars;b++) {
            int y = 2 + b * (h-4) / bars;
            for (int x=2;x<w-2;x++) {
                if ((x + b*3) % 7 >= gap) s_block[NR_IDX(x,y)] = 1;
            }
        }
        if (style >= 8) {
            for (int y=2;y<h-2;y++) {
                if ((y*3 + style) % 5 == 0) {
                    int x = 3 + ((y*7 + style) % (w-6));
                    s_block[NR_IDX(x,y)] = 1;
                }
            }
        }
    }

    /* Always keep a playable central launch area and clear four edge lanes. */
    for (int y=cy-1;y<=cy+1;y++) for (int x=cx-2;x<=cx+2;x++)
        if (in_bounds(x,y)) s_block[NR_IDX(x,y)] = 0;
    for (int y=1;y<h-1;y++) { s_block[NR_IDX(1,y)] = 0; s_block[NR_IDX(w-2,y)] = 0; }
}

static nr_cell_t find_free_cell(int tries)
{
    nr_cell_t p = {1,1};
    for (int i=0;i<tries;i++) {
        p.x = (int8_t)(1 + (nr_rand() % (s_fw > 2 ? s_fw-2 : 1)));
        p.y = (int8_t)(1 + (nr_rand() % (s_fh > 2 ? s_fh-2 : 1)));
        if (cell_blocked(p.x,p.y)) continue;
        if (p.x == s_player.p.x && p.y == s_player.p.y) continue;
        if (occupied_by_enemy(p.x,p.y,-1)) continue;
        if (occupied_by_pickup(p.x,p.y)) continue;
        if (occupied_by_generator(p.x,p.y)) continue;
        return p;
    }
    return p;
}

static void add_particle(nr_cell_t p, uint8_t color_kind)
{
    for (int i=0;i<NR_MAX_PARTICLES;i++) if (!s_particles[i].active) {
        s_particles[i].active = true;
        s_particles[i].p = p;
        s_particles[i].life = 5;
        s_particles[i].color_kind = color_kind;
        return;
    }
}

static void add_pickup(uint8_t type, nr_cell_t p, uint32_t now, uint32_t ttl)
{
    for (int i=0;i<NR_MAX_PICKUPS;i++) if (!s_pickups[i].active) {
        s_pickups[i].active = true;
        s_pickups[i].p = p;
        s_pickups[i].type = type;
        s_pickups[i].until = now + ttl;
        return;
    }
}

static void spawn_player_support(uint32_t now)
{
    if (s_game.level == 2 || s_game.level == 12) {
        for (uint8_t i=0;i<3;i++) add_pickup(PICK_ENERGY, find_free_cell(30), now, 12000);
    } else if (s_game.level % 3 == 0) {
        add_pickup(PICK_SHIELD, find_free_cell(30), now, 12000);
        add_pickup(PICK_SCORE, find_free_cell(30), now, 12000);
    }
}

static void spawn_enemies(const nr_level_cfg_t *L, uint32_t now)
{
    memset(s_enemies, 0, sizeof(s_enemies));
    int n=0;
    for (int i=0;i<L->enemy_count && n<NR_MAX_ENEMIES;i++) {
        nr_enemy_type_t t = EN_DRONE;
        if (i < L->hunter_count) t = EN_HUNTER;
        else if (i < L->hunter_count + L->shooter_count) t = EN_SHOOTER;
        else if (i < L->hunter_count + L->shooter_count + L->fast_count) t = EN_FAST;
        else if (i < L->hunter_count + L->shooter_count + L->fast_count + L->shield_count) t = EN_SHIELD;
        s_enemies[n].active = true;
        s_enemies[n].type = t;
        s_enemies[n].p = find_free_cell(80);
        s_enemies[n].dir = (nr_dir_t)(nr_rand()%4);
        s_enemies[n].hp = (t == EN_SHIELD ? 2 : 1);
        s_enemies[n].shield_dir = (uint8_t)(nr_rand()%4);
        s_enemies[n].next_move = now + 300 + (nr_rand()%400);
        s_enemies[n].next_fire = now + 900 + (nr_rand()%800);
        n++;
    }
    if (L->boss && n < NR_MAX_ENEMIES) {
        nr_cell_t boss_p = find_free_cell(160);
        s_enemies[n].active = true;
        s_enemies[n].type = EN_BOSS;
        s_enemies[n].p = boss_p;
        s_enemies[n].dir = DIR_LEFT;
        s_enemies[n].hp = (uint8_t)(L->boss_hp > 255 ? 255 : L->boss_hp);
        s_enemies[n].next_move = now + 350;
        s_enemies[n].next_fire = now + 500;
    }
}

static void spawn_generators(void)
{
    memset(s_generators,0,sizeof(s_generators));
    int w=s_fw,h=s_fh;
    nr_cell_t p[3] = {
        {(int8_t)2,(int8_t)2},
        {(int8_t)(w-3),(int8_t)(h-3)},
        {(int8_t)(w-3),(int8_t)2}
    };
    for (int i=0;i<3;i++) {
        if (cell_blocked(p[i].x,p[i].y)) p[i].x = (int8_t)(w/2 + (i-1)*3);
        s_generators[i].active = true;
        s_generators[i].p = p[i];
        s_generators[i].hp = 3;
    }
}

static void reset_player(uint32_t now)
{
    s_player.p = (nr_cell_t){(int8_t)(s_fw/2),(int8_t)(s_fh/2)};
    s_player.move_dir = DIR_RIGHT;
    s_player.aim_dir = DIR_RIGHT;
    s_player.hp = s_player.max_hp = 5;
    s_player.energy = 100;
    s_player.weapon = W_PULSE;
    s_player.next_move = now + 250;
    s_player.next_fire = now + 200;
    s_player.next_dash = now;
    s_player.shield_until = 0;
    s_player.shield_cd_until = 0;
    s_player.emp_cd_until = 0;
    s_player.laser_cd_until = 0;
    s_player.hit_iframe_until = 0;
    s_player.charge_ms = 0;
    s_player.lt_was_held = false;
    s_player.kills = 0;
    s_player.combo = 0;
    s_player.combo_until = 0;
}

static void begin_level(uint8_t level, uint32_t now, bool keep_score)
{
    if (level < 1) level=1;
    if (level > 20) level=20;
    s_game.level = level;
    s_game.objective_now = 0;
    s_game.elapsed_ms = 0;
    s_game.clear_t0 = 0;
    s_game.over_t0 = 0;
    s_game.core_hp = s_levels[level-1].core_hp;
    s_game.gate_open = false;
    s_level_t0 = now;
    s_last_ai = now;
    s_last_render = 0;
    s_rng ^= (uint32_t)level * 0x9E3779B9u;
    s_fw = matrix_field_w();
    s_fh = matrix_field_h();
    if (s_fw > NR_W_MAX) s_fw = NR_W_MAX;
    if (s_fh > NR_H_MAX) s_fh = NR_H_MAX;
    build_level_geometry(s_levels[level-1].obstacle_style);
    memset(s_bullets,0,sizeof(s_bullets));
    memset(s_pickups,0,sizeof(s_pickups));
    memset(s_particles,0,sizeof(s_particles));
    memset(s_generators,0,sizeof(s_generators));
    reset_player(now);
    spawn_enemies(&s_levels[level-1], now);
    if (s_levels[level-1].obj == OBJ_GENERATORS) spawn_generators();
    spawn_player_support(now);
    if (!keep_score) {
        s_game.score = 0;
        s_player.kills = 0;
    }
}

static nr_dir_t opposite_dir(nr_dir_t d)
{
    switch (d) {
    case DIR_UP: return DIR_DOWN;
    case DIR_DOWN: return DIR_UP;
    case DIR_LEFT: return DIR_RIGHT;
    case DIR_RIGHT: return DIR_LEFT;
    }
    return DIR_UP;
}

static void score_kill(uint32_t now, uint8_t bonus)
{
    if (s_player.combo_until && now <= s_player.combo_until) {
        if (s_player.combo < 5) s_player.combo++;
    } else {
        s_player.combo = 1;
    }
    s_player.combo_until = now + NR_COMBO_WINDOW_MS;
    uint32_t mult = s_levels[s_game.level-1].score_mult + (s_player.combo > 1 ? s_player.combo-1 : 0);
    s_game.score += 25u * mult + bonus;
}

static void kill_enemy(int i, uint32_t now, uint8_t bonus)
{
    if (!s_enemies[i].active) return;
    nr_cell_t p=s_enemies[i].p;
    bool boss = s_enemies[i].type == EN_BOSS;
    if (boss) return;
    s_enemies[i].active=false;
    s_player.kills++;
    score_kill(now, bonus);
    add_particle(p,1);
    if ((s_player.kills % 4)==0) add_pickup((uint8_t)(nr_rand()%3), find_free_cell(40), now, 9000);
    snd_play("score_tick");
}

static int find_bullet_slot(void)
{
    for (int i=0;i<NR_MAX_BULLETS;i++) if (!s_bullets[i].active) return i;
    return -1;
}

static void fire_bullet(nr_cell_t p, nr_dir_t d, bool from_player, uint8_t dmg, uint8_t ttl, uint32_t now)
{
    int i=find_bullet_slot();
    if (i<0) return;
    s_bullets[i].active=true;
    s_bullets[i].from_player=from_player;
    s_bullets[i].p=p;
    s_bullets[i].dir=d;
    s_bullets[i].damage=dmg;
    s_bullets[i].ttl=ttl;
    s_bullets[i].next_step=now+NR_BULLET_MS;
}

static void fire_current_weapon(uint32_t now, uint16_t charge)
{
    if (now < s_player.next_fire) return;
    nr_cell_t p=ahead(s_player.p,s_player.aim_dir);
    uint8_t dmg=1;
    uint16_t cd=150;
    if (s_player.weapon == W_PULSE) {
        dmg = (charge >= 700 ? 2 : 1);
        fire_bullet(p,s_player.aim_dir,true,dmg,10,now);
        cd = 130;
    } else if (s_player.weapon == W_CANNON) {
        dmg = (uint8_t)(2 + (charge/350));
        if (dmg>4) dmg=4;
        fire_bullet(p,s_player.aim_dir,true,dmg,12,now);
        cd = 300;
        add_particle(p,2);
    } else {
        fire_bullet(p,s_player.aim_dir,true,1,8,now);
        nr_dir_t d=s_player.aim_dir;
        if (d==DIR_UP || d==DIR_DOWN) {
            nr_cell_t q=p; q.x += 1; fire_bullet(q,d,true,1,8,now);
            q=p; q.x -= 1; fire_bullet(q,d,true,1,8,now);
        } else {
            nr_cell_t q=p; q.y += 1; fire_bullet(q,d,true,1,8,now);
            q=p; q.y -= 1; fire_bullet(q,d,true,1,8,now);
        }
        cd = 240;
    }
    s_player.next_fire = now + cd;
    snd_play("laser");
}

static void use_emp(uint32_t now)
{
    if (now < s_player.emp_cd_until || s_player.energy < 35) return;
    s_player.energy -= 35;
    s_player.emp_cd_until = now + NR_EMP_CD_MS;
    s_last_event = 2;
    for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active) {
        int dx=s_enemies[i].p.x-s_player.p.x; if (dx<0) dx=-dx;
        int dy=s_enemies[i].p.y-s_player.p.y; if (dy<0) dy=-dy;
        if (dx+dy <= 5) {
            if (s_enemies[i].type==EN_BOSS) {
                if (s_enemies[i].hp>2) s_enemies[i].hp-=2; else s_enemies[i].hp=0;
            } else {
                kill_enemy(i,now,20);
            }
        }
    }
    add_particle(s_player.p,3);
    snd_play("impact_moderate");
}

static void use_shield(uint32_t now)
{
    if (now < s_player.shield_cd_until || s_player.energy < 20) return;
    s_player.energy -= 20;
    s_player.shield_until = now + NR_SHIELD_MS;
    s_player.shield_cd_until = now + NR_SHIELD_CD_MS;
    s_last_event = 3;
    snd_play("sys_ready");
}

static void use_dash(uint32_t now)
{
    if (now < s_player.next_dash) return;
    nr_cell_t q=s_player.p;
    for (int n=0;n<3;n++) {
        nr_cell_t t=ahead(q,s_player.move_dir);
        if (!in_bounds(t.x,t.y) || cell_blocked(t.x,t.y) || occupied_by_enemy(t.x,t.y,-1)) break;
        q=t;
    }
    if (q.x!=s_player.p.x || q.y!=s_player.p.y) {
        s_player.p=q;
        s_player.next_dash=now+NR_DASH_MS;
        s_player.hit_iframe_until=now+NR_DASH_MS/2;
        add_particle(q,2);
    }
}

static void use_laser(uint32_t now)
{
    if (now < s_player.laser_cd_until || s_player.energy < 25) return;
    s_player.energy -= 25;
    s_player.laser_cd_until = now + NR_LASER_CD_MS;
    nr_cell_t q=s_player.p;
    for (int n=0;n<9;n++) {
        q=ahead(q,s_player.aim_dir);
        if (!in_bounds(q.x,q.y) || cell_blocked(q.x,q.y)) break;
        add_particle(q,4);
        for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active && s_enemies[i].p.x==q.x && s_enemies[i].p.y==q.y) {
            if (s_enemies[i].type==EN_BOSS) {
                if (s_enemies[i].hp>2) s_enemies[i].hp-=2; else s_enemies[i].hp=0;
            } else kill_enemy(i,now,40);
        }
    }
    snd_play("laser");
}

static void damage_player(uint8_t dmg, uint32_t now)
{
    if (now < s_player.hit_iframe_until) return;
    if (now < s_player.shield_until) {
        s_player.hit_iframe_until = now + 150;
        add_particle(s_player.p,3);
        return;
    }
    s_player.hp = s_player.hp > dmg ? (uint8_t)(s_player.hp-dmg) : 0;
    s_player.hit_iframe_until = now + NR_HIT_IFRAME_MS;
    s_player.combo = 0;
    s_player.combo_until = 0;
    add_particle(s_player.p,1);
    snd_play("impact_light");
    if (s_player.hp==0) {
        if (s_game.lives>1) {
            s_game.lives--;
            s_state = NR_PAUSED;
            s_state_t0 = now;
            s_pad_lost = false;
        } else {
            s_state = NR_OVER;
            s_game.over_t0=(uint16_t)(now & 0xFFFF);
            if (s_game.score>s_hi) { s_hi=s_game.score; snd_play("highscore_fanfare"); }
            else snd_play("game_over");
        }
    }
}

static void update_pickups(uint32_t now)
{
    for (int i=0;i<NR_MAX_PICKUPS;i++) if (s_pickups[i].active) {
        if ((int32_t)(now-s_pickups[i].until)>=0) { s_pickups[i].active=false; continue; }
        if (s_pickups[i].p.x==s_player.p.x && s_pickups[i].p.y==s_player.p.y) {
            if (s_pickups[i].type==PICK_ENERGY) s_player.energy = (uint8_t)(s_player.energy>75?100:s_player.energy+25);
            else if (s_pickups[i].type==PICK_SCORE) s_game.score += 100u * s_levels[s_game.level-1].score_mult;
            else s_player.shield_until = now + NR_SHIELD_MS;
            add_particle(s_player.p,5);
            snd_play("score_reveal");
            s_pickups[i].active=false;
            if (s_levels[s_game.level-1].obj == OBJ_COLLECT) s_game.objective_now++;
        }
    }
}

static void update_generators(uint32_t now)
{
    (void)now;
    if (s_levels[s_game.level-1].obj != OBJ_GENERATORS) return;
    uint8_t count=0;
    for (int i=0;i<NR_MAX_GENERATORS;i++) if (!s_generators[i].active) count++;
    s_game.objective_now = count;
    if (count >= 3) return;
}

static void enemy_shoot(nr_enemy_t *e, uint32_t now)
{
    nr_dir_t d=e->dir;
    int dx=s_player.p.x-e->p.x, dy=s_player.p.y-e->p.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx>=dy) d=(s_player.p.x>e->p.x)?DIR_RIGHT:DIR_LEFT;
    else d=(s_player.p.y>e->p.y)?DIR_UP:DIR_DOWN;
    fire_bullet(ahead(e->p,d),d,false,1,8,now);
}

static void update_enemy_ai(uint32_t now)
{
    if (now-s_last_ai < NR_ENEMY_AI_MS) return;
    s_last_ai=now;
    const nr_level_cfg_t *L=&s_levels[s_game.level-1];

    for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active) {
        nr_enemy_t *e=&s_enemies[i];
        if (e->type==EN_BOSS) {
            if (now>=e->next_move) {
                e->next_move=now+300;
                int dx=s_player.p.x-e->p.x, dy=s_player.p.y-e->p.y;
                nr_dir_t d=(abs(dx)>=abs(dy))?(dx>0?DIR_RIGHT:DIR_LEFT):(dy>0?DIR_UP:DIR_DOWN);
                nr_cell_t q=ahead(e->p,d);
                if (in_bounds(q.x,q.y)&&!cell_blocked(q.x,q.y)) e->p=q;
            }
            if (now>=e->next_fire) {
                e->next_fire=now+650;
                enemy_shoot(e,now);
            }
            continue;
        }
        if (now < e->next_move) continue;
        e->next_move = now + L->enemy_step_ms - (e->type==EN_FAST?80:0);
        nr_dir_t d=e->dir;
        if (e->type==EN_HUNTER || e->type==EN_SHOOTER) {
            int dx=s_player.p.x-e->p.x, dy=s_player.p.y-e->p.y;
            if (e->type==EN_SHOOTER) {
                if (abs(dx)+abs(dy)<4) d=(abs(dx)>=abs(dy))?(dx>0?DIR_LEFT:DIR_RIGHT):(dy>0?DIR_DOWN:DIR_UP);
                else d=(abs(dx)>=abs(dy))?(dx>0?DIR_RIGHT:DIR_LEFT):(dy>0?DIR_UP:DIR_DOWN);
            } else {
                d=(abs(dx)>=abs(dy))?(dx>0?DIR_RIGHT:DIR_LEFT):(dy>0?DIR_UP:DIR_DOWN);
            }
        } else if (e->type==EN_FAST) {
            if ((nr_rand()&3)==0) d=(nr_dir_t)(nr_rand()%4);
        } else if ((nr_rand()&3)==0) {
            d=(nr_dir_t)(nr_rand()%4);
        }
        nr_cell_t q=ahead(e->p,d);
        if (!in_bounds(q.x,q.y) || cell_blocked(q.x,q.y) || occupied_by_enemy(q.x,q.y,i)) {
            d=(nr_dir_t)(nr_rand()%4);
            q=ahead(e->p,d);
        }
        if (in_bounds(q.x,q.y)&&!cell_blocked(q.x,q.y)&&!occupied_by_enemy(q.x,q.y,i)) {
            e->p=q; e->dir=d;
        }
        if ((e->type==EN_SHOOTER || e->type==EN_BOSS) && now>=e->next_fire) {
            e->next_fire=now+1200;
            enemy_shoot(e,now);
        }
        if (e->p.x==s_player.p.x && e->p.y==s_player.p.y) damage_player(1,now);
    }

    if (L->obj == OBJ_DEFEND) {
        /* Defend the core: core takes damage when enemies reach its cell. */
        nr_cell_t core={(int8_t)(s_fw/2),(int8_t)(s_fh/2)};
        for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active && s_enemies[i].p.x==core.x && s_enemies[i].p.y==core.y) {
            if (s_game.core_hp>15) s_game.core_hp-=15; else s_game.core_hp=0;
            s_enemies[i].active=false;
        }
    }

    /* Timed/wave levels gain replacements until the target is reached. */
    if ((L->obj==OBJ_DEFEND || L->obj==OBJ_SURVIVE) && now-s_level_t0 > 1200) {
        int active=0;
        for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active) active++;
        if (active < NR_MAX_ENEMIES && ((now-s_level_t0)/1000u)%2u==0) {
            static uint32_t last_spawn=0;
            if (now-last_spawn>900) {
                last_spawn=now;
                for (int i=0;i<NR_MAX_ENEMIES;i++) if (!s_enemies[i].active) {
                    s_enemies[i].active=true; s_enemies[i].type=(nr_enemy_type_t)(nr_rand()%4);
                    s_enemies[i].p=find_free_cell(60); s_enemies[i].hp=1;
                    s_enemies[i].dir=(nr_dir_t)(nr_rand()%4);
                    s_enemies[i].next_move=now+300; s_enemies[i].next_fire=now+1000;
                    break;
                }
            }
        }
    }
}

static void update_bullets(uint32_t now)
{
    for (int i=0;i<NR_MAX_BULLETS;i++) if (s_bullets[i].active) {
        nr_bullet_t *b=&s_bullets[i];
        if (now<b->next_step) continue;
        b->next_step=now+NR_BULLET_MS;
        if (b->ttl==0) { b->active=false; continue; }
        b->ttl--;
        if (!in_bounds(b->p.x,b->p.y) || cell_blocked(b->p.x,b->p.y)) { b->active=false; continue; }

        if (b->from_player) {
            bool hit=false;
            for (int e=0;e<NR_MAX_ENEMIES;e++) if (s_enemies[e].active && s_enemies[e].p.x==b->p.x && s_enemies[e].p.y==b->p.y) {
                hit=true;
                if (s_enemies[e].type==EN_BOSS) {
                    s_enemies[e].hp = s_enemies[e].hp>b->damage ? (uint8_t)(s_enemies[e].hp-b->damage) : 0;
                } else if (s_enemies[e].type==EN_SHIELD) {
                    if (s_enemies[e].shield_dir != opposite_dir(b->dir)) {
                        s_enemies[e].hp = s_enemies[e].hp>b->damage ? (uint8_t)(s_enemies[e].hp-b->damage):0;
                    }
                } else {
                    s_enemies[e].hp = s_enemies[e].hp>b->damage ? (uint8_t)(s_enemies[e].hp-b->damage):0;
                }
                if (s_enemies[e].hp==0) {
                    if (s_enemies[e].type==EN_BOSS) {
                        s_state=NR_CLEAR; s_state_t0=now;
                        s_game.clear_t0=(uint16_t)(now&0xFFFF);
                        if (s_game.level < 20 && s_game.level >= s_unlocked) s_unlocked=(uint8_t)(s_game.level+1);
                        s_game.score += 1000u*s_levels[s_game.level-1].score_mult;
                        snd_play("highscore_fanfare");
                    } else kill_enemy(e,now,20);
                }
                break;
            }
            for (int g=0;g<NR_MAX_GENERATORS;g++) if (s_generators[g].active && s_generators[g].p.x==b->p.x && s_generators[g].p.y==b->p.y) {
                hit=true;
                if (s_generators[g].hp>b->damage) s_generators[g].hp-=b->damage; else s_generators[g].hp=0;
                if (s_generators[g].hp==0) { s_generators[g].active=false; s_game.score+=200; add_particle(s_generators[g].p,4); snd_play("score_tick"); }
                break;
            }
            if (hit) { b->active=false; continue; }
        } else {
            if (b->p.x==s_player.p.x && b->p.y==s_player.p.y) {
                damage_player(b->damage,now); b->active=false; continue;
            }
        }
        step_cell(&b->p,b->dir);
    }
}

static bool objective_complete(uint32_t now)
{
    const nr_level_cfg_t *L=&s_levels[s_game.level-1];
    switch (L->obj) {
    case OBJ_CLEAR: {
        uint16_t kills=0; for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active) kills++;
        s_game.objective_now = (uint8_t)(s_player.kills > 255 ? 255 : s_player.kills);
        return s_player.kills >= L->obj_target;
    }
    case OBJ_COLLECT:
        return s_game.objective_now >= L->obj_target;
    case OBJ_DEFEND:
        s_game.objective_now=(uint8_t)((now-s_level_t0)/1000u);
        return s_game.core_hp>0 && now-s_level_t0 >= NR_SURVIVE_MS;
    case OBJ_REACH:
        s_game.gate_open = s_player.kills >= L->obj_target;
        s_game.objective_now=(uint8_t)(s_player.kills>255?255:s_player.kills);
        return s_game.gate_open && (s_player.p.x == (int8_t)(s_fw-2) || s_player.p.x==1) &&
               (s_player.p.y == 1 || s_player.p.y==(int8_t)(s_fh-2));
    case OBJ_GENERATORS:
        s_game.objective_now=0; for (int i=0;i<3;i++) if (!s_generators[i].active) s_game.objective_now++;
        return s_game.objective_now>=L->obj_target;
    case OBJ_SURVIVE:
        s_game.objective_now=(uint8_t)((now-s_level_t0)/1000u);
        return now-s_level_t0 >= (uint32_t)L->obj_target*1000u;
    case OBJ_BOSS:
        s_game.objective_now=0;
        for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active && s_enemies[i].type==EN_BOSS) s_game.objective_now=1;
        return s_game.objective_now==0;
    }
    return false;
}

static void update_player(uint32_t now)
{
    if (now < s_player.next_move) return;
    nr_cell_t q=ahead(s_player.p,s_player.move_dir);
    if (in_bounds(q.x,q.y) && !cell_blocked(q.x,q.y) && !occupied_by_enemy(q.x,q.y,-1)) {
        s_player.p=q;
        s_player.next_move=now+NR_MOVE_MS;
        for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active && s_enemies[i].p.x==q.x && s_enemies[i].p.y==q.y) damage_player(1,now);
        update_pickups(now);
    } else {
        s_player.next_move=now+70;
    }
}

static void render_level_border(uint32_t now)
{
    rgb_t c1 = (rgb_t){0,52,95};
    rgb_t c2 = (rgb_t){0,82,90};
    uint8_t phase=(uint8_t)((now/220u)&1u);
    for (int x=0;x<s_fw;x++) {
        px(x,0, phase?c2:c1); px(x,s_fh-1,phase?c1:c2);
    }
    for (int y=0;y<s_fh;y++) {
        px(0,y, (y&1)?c1:c2); px(s_fw-1,y,(y&1)?c2:c1);
    }
}

static void render_game(uint32_t now)
{
    clear_frame();
    const nr_level_cfg_t *L=&s_levels[s_game.level-1];

    /* low-intensity ambient field */
    for (int i=0;i<14;i++) {
        int x=(i*11+(now/260u))%s_fw;
        int y=(i*7+(now/410u))%s_fh;
        px(x,y,(rgb_t){4,10,18});
    }
    render_level_border(now);

    /* obstacle field */
    for (int y=1;y<s_fh-1;y++) for (int x=1;x<s_fw-1;x++) if (cell_blocked(x,y)) {
        px(x,y,(rgb_t){10,24,48});
    }

    /* objective core */
    if (L->obj==OBJ_DEFEND) {
        nr_cell_t c={(int8_t)(s_fw/2),(int8_t)(s_fh/2)};
        uint8_t glow=(uint8_t)(55 + ((now/140)&1)*25);
        px(c.x,c.y,(rgb_t){glow,glow,0});
        px(c.x-1,c.y,(rgb_t){20,45,0}); px(c.x+1,c.y,(rgb_t){20,45,0});
        px(c.x,c.y-1,(rgb_t){20,45,0}); px(c.x,c.y+1,(rgb_t){20,45,0});
    }

    /* generators */
    for (int i=0;i<3;i++) if (s_generators[i].active) {
        uint8_t b=(uint8_t)(90+((now/180u+i*40u)%80));
        px(s_generators[i].p.x,s_generators[i].p.y,(rgb_t){b,20,b});
    }

    /* exit gate */
    if (L->obj==OBJ_REACH) {
        bool open=s_game.gate_open;
        rgb_t c=open?(rgb_t){40,230,100}:(rgb_t){90,30,20};
        px(1,1,c); px(1,s_fh-2,c); px(s_fw-2,1,c); px(s_fw-2,s_fh-2,c);
    }

    /* pickups */
    for (int i=0;i<NR_MAX_PICKUPS;i++) if (s_pickups[i].active) {
        bool on=((now/180u)&1u)==0;
        if (!on) continue;
        rgb_t c=(s_pickups[i].type==PICK_ENERGY)?(rgb_t){30,190,255}:
                 (s_pickups[i].type==PICK_SCORE)?(rgb_t){255,170,30}:
                 (rgb_t){100,80,255};
        px(s_pickups[i].p.x,s_pickups[i].p.y,c);
    }

    /* enemy projectiles */
    for (int i=0;i<NR_MAX_BULLETS;i++) if (s_bullets[i].active) {
        px(s_bullets[i].p.x,s_bullets[i].p.y,s_bullets[i].from_player?(rgb_t){120,255,240}:(rgb_t){255,50,35});
    }

    /* enemies */
    for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active) {
        nr_enemy_t *e=&s_enemies[i];
        rgb_t c;
        switch(e->type) {
        case EN_DRONE: c=(rgb_t){255,50,70}; break;
        case EN_HUNTER: c=(rgb_t){255,90,20}; break;
        case EN_SHOOTER: c=(rgb_t){220,40,255}; break;
        case EN_FAST: c=(rgb_t){255,190,20}; break;
        case EN_SHIELD: c=(rgb_t){100,120,255}; break;
        default: c=(rgb_t){255,0,170}; break;
        }
        if (e->type==EN_BOSS) {
            px(e->p.x,e->p.y,c);
            px(e->p.x-1,e->p.y,scale_rgb(c,65)); px(e->p.x+1,e->p.y,scale_rgb(c,65));
            px(e->p.x,e->p.y-1,scale_rgb(c,65)); px(e->p.x,e->p.y+1,scale_rgb(c,65));
        } else px(e->p.x,e->p.y,c);
    }

    /* player */
    bool shield = now < s_player.shield_until;
    rgb_t pc=shield?(rgb_t){70,210,255}:(rgb_t){70,255,200};
    px(s_player.p.x,s_player.p.y,pc);
    nr_cell_t nose=ahead(s_player.p,s_player.aim_dir);
    px(nose.x,nose.y,(rgb_t){180,255,255});
    if (shield) {
        nr_cell_t a=s_player.p; a.x++; px(a.x,a.y,(rgb_t){30,90,120});
        a=s_player.p; a.x--; px(a.x,a.y,(rgb_t){30,90,120});
        a=s_player.p; a.y++; px(a.x,a.y,(rgb_t){30,90,120});
        a=s_player.p; a.y--; px(a.x,a.y,(rgb_t){30,90,120});
    }

    /* particles */
    for (int i=0;i<NR_MAX_PARTICLES;i++) if (s_particles[i].active) {
        rgb_t c=(s_particles[i].color_kind==1)?(rgb_t){255,70,40}:
                 (s_particles[i].color_kind==2)?(rgb_t){255,200,40}:
                 (s_particles[i].color_kind==3)?(rgb_t){50,130,255}:
                 (rgb_t){255,120,220};
        px(s_particles[i].p.x,s_particles[i].p.y,scale_rgb(c,(uint8_t)(40+s_particles[i].life*12)));
    }

    matrix_show();
}

static void update_particles(void)
{
    for (int i=0;i<NR_MAX_PARTICLES;i++) if (s_particles[i].active) {
        if (s_particles[i].life>0) s_particles[i].life--; else s_particles[i].active=false;
    }
}

static void render_menu(uint32_t now)
{
    clear_frame();
    for (int x=0;x<s_fw;x++) { px(x,0,(rgb_t){0,60,100}); px(x,s_fh-1,(rgb_t){0,35,65}); }
    /* stylized ship */
    int cx=s_fw/2, cy=s_fh/2;
    px(cx,cy,(rgb_t){80,255,220}); px(cx-1,cy,(rgb_t){40,180,170}); px(cx+1,cy,(rgb_t){40,180,170});
    px(cx,cy+1,(rgb_t){20,80,80});
    for (int i=0;i<3;i++) {
        int x=3+i*2;
        px(x,1,(i==(s_selected_level-1)%3)?(rgb_t){255,170,40}:(rgb_t){30,60,80});
    }
    /* level marker bar */
    int n=s_selected_level<10? s_selected_level : 10;
    for (int i=0;i<n && i<s_fw-4;i++) px(2+i,s_fh-2,(rgb_t){40,160,230});
    matrix_show();
    (void)now;
}

static void render_countdown(uint32_t now)
{
    clear_frame();
    uint32_t el=now-s_state_t0;
    uint8_t n=(uint8_t)(3-(el/650));
    if (el<1950) {
        int w = n==3?3:(n==2?3:3);
        int cx=s_fw/2; int cy=s_fh/2;
        if (n==3) { px(cx-1,cy-2,(rgb_t){0,220,255}); px(cx,cy-2,(rgb_t){0,220,255}); px(cx+1,cy-2,(rgb_t){0,220,255}); px(cx+1,cy-1,(rgb_t){0,220,255}); px(cx-1,cy,(rgb_t){0,220,255}); px(cx,cy,(rgb_t){0,220,255}); px(cx+1,cy+1,(rgb_t){0,220,255}); px(cx-1,cy+2,(rgb_t){0,220,255}); px(cx,cy+2,(rgb_t){0,220,255}); px(cx+1,cy+2,(rgb_t){0,220,255}); }
        else if (n==2) { for(int x=cx-1;x<=cx+1;x++){px(x,cy-2,(rgb_t){255,190,40});px(x,cy,(rgb_t){255,190,40});px(x,cy+2,(rgb_t){255,190,40});} px(cx-1,cy-1,(rgb_t){255,190,40});px(cx+1,cy+1,(rgb_t){255,190,40}); }
        else { for(int x=cx-1;x<=cx+1;x++){px(x,cy-2,(rgb_t){255,70,50});px(x,cy+2,(rgb_t){255,70,50});} px(cx,cy,(rgb_t){255,70,50}); }
        (void)w;
    } else {
        for(int x=0;x<s_fw;x++){px(x,0,(rgb_t){0,180,90});px(x,s_fh-1,(rgb_t){0,180,90});}
    }
    matrix_show();
}

static void render_pause(uint32_t now)
{
    render_game(now);
    /* dim is handled by driver after frame composition */
    matrix_set_dim_pct(35);
    matrix_show();
}

static void render_clear(uint32_t now)
{
    clear_frame();
    uint32_t el=now-s_state_t0;
    int sweep=(int)(el/70u);
    for (int y=0;y<s_fh;y++) for (int x=0;x<s_fw;x++) {
        if (x+y <= sweep) px(x,y,(rgb_t){40,230,160});
    }
    matrix_show();
}

static void render_over(uint32_t now)
{
    clear_frame();
    uint32_t el=now-s_state_t0;
    bool flash=((el/240u)&1u)==0;
    rgb_t c=flash?(rgb_t){255,40,45}:(rgb_t){80,10,25};
    for(int x=0;x<s_fw;x++){px(x,0,c);px(x,s_fh-1,c);}
    for(int y=0;y<s_fh;y++){px(0,y,c);px(s_fw-1,y,c);}
    int bars=(int)((s_game.score/100u) % (s_fw-4));
    for(int i=0;i<bars;i++) px(2+i,s_fh/2,(rgb_t){255,170,40});
    matrix_show();
}

static void state_tick(uint32_t now)
{
    switch (s_state) {
    case NR_MENU:
        if (now-s_last_render>=NR_FRAME_MS) { s_last_render=now; render_menu(now); }
        break;
    case NR_COUNTDOWN:
        if (now-s_last_render>=NR_FRAME_MS) { s_last_render=now; render_countdown(now); }
        if (now-s_state_t0>=2150) { s_state=NR_PLAY; s_state_t0=now; s_player.next_move=now+200; matrix_set_dim_pct(100); }
        break;
    case NR_PLAY:
        if (s_game.elapsed_ms > 65000) s_game.elapsed_ms=65000;
        if (now-s_level_t0>65000) s_game.elapsed_ms=65000; else s_game.elapsed_ms=(uint16_t)(now-s_level_t0);
        update_bullets(now);
        update_enemy_ai(now);
        if (s_levels[s_game.level-1].obj == OBJ_DEFEND && s_game.core_hp == 0) {
            s_state = NR_OVER;
            s_state_t0 = now;
            break;
        }
        update_player(now);
        update_pickups(now);
        update_particles();
        if (s_player.combo_until && now>s_player.combo_until) { s_player.combo=0; s_player.combo_until=0; }
        if (objective_complete(now) && now-s_state_t0>300) {
            s_state=NR_CLEAR; s_state_t0=now;
            if (s_game.level < 20 && s_game.level >= s_unlocked) s_unlocked=s_game.level+1;
            s_game.score += 150u*s_game.level + s_player.combo*25u;
            snd_play("highscore_fanfare");
        }
        if (now-s_last_render>=NR_FRAME_MS) { s_last_render=now; render_game(now); }
        break;
    case NR_PAUSED:
        if (now-s_last_render>=100) { s_last_render=now; render_pause(now); }
        break;
    case NR_CLEAR:
        if (now-s_last_render>=NR_FRAME_MS) { s_last_render=now; render_clear(now); }
        if (now-s_state_t0>=1300) {
            if (s_game.level < 20) {
                begin_level((uint8_t)(s_game.level+1),now,true);
                s_state=NR_COUNTDOWN; s_state_t0=now;
            } else {
                /* Endless: replay level 20 with slightly faster enemies, score retained. */
                begin_level(20,now,true);
                s_state=NR_COUNTDOWN; s_state_t0=now;
            }
        }
        break;
    case NR_OVER:
        if (now-s_last_render>=NR_FRAME_MS) { s_last_render=now; render_over(now); }
        break;
    default: break;
    }
}

void neon_raid_init(void)
{
    if (s_inited) return;
    memset(&s_game,0,sizeof(s_game));
    memset(&s_player,0,sizeof(s_player));
    s_selected_level=1; s_unlocked=1; s_hi=0; s_state=NR_OFF; s_inited=true;
    matrix_set_dim_pct(100);
}

bool neon_raid_active(void)
{
    return s_state != NR_OFF;
}

uint8_t neon_raid_state(void) { return (uint8_t)s_state; }
bool neon_raid_suppress_oled(void) { return s_state==NR_COUNTDOWN || s_state==NR_PLAY || s_state==NR_CLEAR; }

bool neon_raid_open(void)
{
    neon_raid_init();
    if (!led_matrix_ready() || g.estop || !os_parked() || os_game_active()) return false;
    if (!led_matrix_acquire(OWNER_ARCADE)) return false;
    s_fw=matrix_field_w(); s_fh=matrix_field_h();
    if (s_fw > NR_W_MAX) s_fw = NR_W_MAX;
    if (s_fh > NR_H_MAX) s_fh = NR_H_MAX;
    s_state=NR_MENU; s_state_t0=(uint32_t)(esp_timer_get_time()/1000ULL); s_last_render=0;
    s_back_t0=0; matrix_set_dim_pct(100); led_matrix_set_dim(false); led_matrix_set_demo(false);
    input_flush_context_switch();
    snd_play("ui_open_sheet");
    notif_push(NOTIF_RULES,"ARCADE: NEON RAID",s_state_t0);
    return true;
}

static void exit_full(uint32_t now)
{
    (void)now;
    s_state=NR_OFF; s_back_t0=0; s_pad_lost=false;
    matrix_set_dim_pct(100);
    led_matrix_safe_off(); led_matrix_release(OWNER_ARCADE); led_matrix_set_demo(true);
    input_flush_context_switch();
    snd_play("ui_back");
    os_ctx_t *oc = car_os_ctx();
    if (oc && oc->state == OS_GAME_4) os_request_screen(oc, OS_GAMES_HUB, now);
}

bool neon_raid_route(const xbox360_pad_t *pad, uint16_t dig, uint16_t tap, uint32_t now)
{
    neon_raid_init();
    if (s_state==NR_OFF) return false;

    if ((dig & B_BACK) != 0) {
        if (!s_back_t0) s_back_t0=now;
        if (now-s_back_t0>=NR_BACK_HOLD_MS) { exit_full(now); return false; }
    } else s_back_t0=0;

    /* The car loop calls its global input handler after we release arcade input. */
    if (tap & B_GUIDE) { exit_full(now); return false; }
    if (g.estop) { exit_full(now); return false; }

    if (!pad || !pad->present) {
        if (s_state==NR_PLAY || s_state==NR_COUNTDOWN) { s_state=NR_PAUSED; s_state_t0=now; s_pad_lost=true; led_matrix_set_dim(true); }
        return true;
    }
    if (s_pad_lost) {
        s_pad_lost=false;
        led_matrix_set_dim(false);
        s_player.charge_ms=0; s_player.lt_was_held=false;
    }

    /* D-pad is intentionally never referenced in this function. */
    nr_dir_t md = dir_from_stick(pad->lx,pad->ly,s_player.move_dir,9000);
    nr_dir_t ad = dir_from_stick(pad->rx,pad->ry,s_player.aim_dir,9000);

    switch (s_state) {
    case NR_MENU:
        if (tap & B_LB) { if (s_selected_level>1) s_selected_level--; snd_play("ui_nav"); }
        if (tap & B_RB) { if (s_selected_level<s_unlocked) s_selected_level++; snd_play("ui_nav"); }
        if (tap & B_START) {
            s_game.lives=3;
            begin_level(s_selected_level,now,false);
            s_state=NR_COUNTDOWN; s_state_t0=now; s_countdown_step=99;
            snd_play("profile_chime_game");
        }
        break;
    case NR_COUNTDOWN:
        s_player.move_dir=md; s_player.aim_dir=ad;
        break;
    case NR_PLAY: {
        s_player.move_dir=md; s_player.aim_dir=ad;
        if (tap & B_B) use_dash(now);
        if (tap & B_X) use_shield(now);
        if (tap & B_Y) use_emp(now);
        if (tap & B_RB) use_laser(now);
        if (tap & B_LB) { s_player.weapon=(nr_weapon_t)((s_player.weapon+1)%W_COUNT); snd_play("ui_nav"); }

        bool lt=(pad->lt>25);
        if (lt) {
            if (s_player.charge_ms<1200) s_player.charge_ms=(uint16_t)(s_player.charge_ms+33);
        } else if (s_player.lt_was_held && s_player.charge_ms>0) {
            fire_current_weapon(now,s_player.charge_ms);
            s_player.charge_ms=0;
        }
        s_player.lt_was_held=lt;

        if ((dig & B_A) || (dig & B_RS)) {
            uint16_t ch=(uint16_t)(lt?s_player.charge_ms:0);
            if (ch==0) ch=1;
            fire_current_weapon(now,ch);
        }
        if (pad->rt>35) {
            nr_weapon_t old=s_player.weapon;
            s_player.weapon=W_PULSE;
            fire_current_weapon(now,1);
            s_player.weapon=old;
        }
        if (tap & B_START) {
            s_state=NR_PAUSED; s_state_t0=now; matrix_set_dim_pct(35);
            snd_play("ui_open_sheet");
        }
        break;
    }
    case NR_PAUSED:
        if (tap & B_START) {
            if (s_player.hp==0) {
                s_player.hp=s_player.max_hp; s_player.energy=100; begin_level(s_game.level,now,true);
            }
            s_state=NR_PLAY; s_state_t0=now; matrix_set_dim_pct(100); snd_play("ui_close_sheet");
        }
        break;
    case NR_OVER:
        if ((tap & B_START) && now-s_state_t0>700) {
            s_game.lives=3;
            begin_level(s_selected_level,now,false);
            s_state=NR_COUNTDOWN; s_state_t0=now; matrix_set_dim_pct(100); snd_play("ui_tap");
        }
        if (tap & B_BACK) { exit_full(now); return false; }
        break;
    default: break;
    }
    return true;
}

void neon_raid_tick(uint32_t now)
{
    if (s_state==NR_OFF) return;
    state_tick(now);
}

void neon_raid_mx_init(void) { neon_raid_init(); }
void neon_raid_mx_update(const xbox360_pad_t *pad, uint32_t now_ms) { (void)pad; (void)now_ms; }
void neon_raid_mx_draw(uint32_t now_ms) { (void)now_ms; }

void neon_raid_get_status(neon_raid_status_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out));
    out->state=(uint8_t)s_state;
    out->score=s_game.score;
    out->hi=s_hi;
    out->level=s_game.level;
    out->lives=s_game.lives;
    out->hp=s_player.hp;
    out->energy=s_player.energy;
    out->combo=s_player.combo;
    out->kills=s_player.kills;
    out->objective=(uint8_t)s_levels[(s_game.level? s_game.level:1)-1].obj;
    out->objective_now=s_game.objective_now;
    out->objective_target=s_levels[(s_game.level? s_game.level:1)-1].obj_target;
    out->weapon=(uint8_t)s_player.weapon;
    out->boss_hp=0;
    for (int i=0;i<NR_MAX_ENEMIES;i++) if (s_enemies[i].active && s_enemies[i].type==EN_BOSS) out->boss_hp=s_enemies[i].hp;
    out->fw=s_fw; out->fh=s_fh;
    memcpy(out->frame_rgb,s_frame,sizeof(out->frame_rgb));
    memcpy(out->mirror_dirty,s_dirty,sizeof(out->mirror_dirty));
    memset(s_dirty,0,sizeof(s_dirty));
}
