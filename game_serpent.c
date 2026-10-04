/*
 * game_serpent.c - NEON SERPENT V2: premium 120-LED arcade snake.
 *
 * Controls during gameplay only:
 *   Y = UP, A = DOWN, X = LEFT, B = RIGHT
 *   D-pad / sticks / LT / RT / LB / RB are ignored.
 *
 * START = start / pause / resume / retry
 * BACK  = tap -> menu, hold -> exit
 * GUIDE remains available to the car E-STOP path and is never consumed here.
 *
 * V2 systems:
 *   20 levels + endless, fixed timestep, smart food, power-ups, combo scoring,
 *   moving hazards, fixed particle pool, richer RGB animation, Xbox haptics,
 *   sound-bank events, NVS career statistics and TFT snapshot data.
 */
#include <stddef.h>
#include "game_serpent.h"
#include "serpent_levels.h"
#include "led_matrix.h"
#include "car_global.h"
#include "car_os.h"
#include "notif.h"
#include "snd_bank.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "input_events.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "serpent";

#define SERPENT_MAX_LEN       80
#define PARTICLE_MAX          16
#define HAZARD_MAX             2
#define BACK_HOLD_MS        1000u
#define FIRST_MOVE_DELAY     500u
#define FRAME_MS              33u
#define COMBO_WINDOW_MS     3500u
#define POWER_SHIELD_MS     12000u
#define POWER_SLOW_MS        6500u
#define POWER_MULTI_MS       7000u
#define POWER_GHOST_MS       5000u
#define ENDLESS_MIN_STEP_MS   78u
#define NVS_HI_KEY         "serp_hi3"
#define NVS_BEST_KEY       "serp_best3"
#define NVS_LONGEST_KEY    "serp_long3"
#define NVS_COMBO_KEY      "serp_combo3"
#define NVS_GAMES_KEY      "serp_games3"
#define NVS_FOODS_KEY      "serp_foods3"
#define NVS_CLEAR_KEY      "serp_clear3"
#define NVS_RUMBLE_KEY     "rumble"

static uint8_t FW = 12, FH = 10;
static uint16_t s_prows[12];
static uint8_t s_corr_lo = 4, s_corr_hi = 6;

static void field_refresh(void)
{
    FW = matrix_field_w();
    FH = matrix_field_h();
    if (FW == 0 || FH == 0 || FW * FH != 120) {
        FW = 12; FH = 10;
    }
}

typedef struct { int8_t x, y; } cell_t;
typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } direction_t;
typedef enum {
    ARC_OFF, ARC_MENU, ARC_COUNTDOWN, ARC_INTRO, ARC_PLAY,
    ARC_PAUSED, ARC_DYING, ARC_RETRY, ARC_CLEAR, ARC_OVER
} arc_state_t;
typedef enum { DEAD_NONE, DEAD_WALL, DEAD_SELF, DEAD_MINE } dead_t;
typedef enum { FOOD_NORMAL = 0, FOOD_GOLD = 1, FOOD_PRISM = 2 } food_kind_t;
typedef enum { POWER_NONE = 0, POWER_SHIELD = 1, POWER_SLOW = 2,
               POWER_MULTI = 3, POWER_GHOST = 4 } power_kind_t;

typedef struct {
    cell_t p;
    int8_t dx, dy;
    uint8_t active;
    uint8_t kind;       /* 1 patrol, 2 predictive hunter */
    uint8_t period;     /* move every N snake steps */
    uint8_t phase;
} hazard_t;

typedef struct {
    int8_t x, y, vx, vy;
    uint8_t r, g, b;
    uint16_t age_ms, life_ms;
    uint8_t active;
} particle_t;

typedef struct {
    cell_t body[SERPENT_MAX_LEN];
    uint8_t length;
    direction_t dir, last_applied;
    direction_t queue[2];
    uint8_t queue_n;

    cell_t food;
    food_kind_t food_kind;
    uint32_t food_t0, food_deadline;

    cell_t power;
    power_kind_t power_kind;
    power_kind_t active_power;
    uint32_t power_until;
    uint8_t shield;

    cell_t mines[6];
    uint8_t mine_count;
    hazard_t hz[HAZARD_MAX];

    uint32_t score;
    uint32_t level_score;
    uint8_t level;
    uint8_t lives;
    uint8_t food_in_level;
    uint8_t endless_food;
    uint8_t special_collected;
    uint8_t level_special_target;

    uint8_t combo;
    uint8_t max_combo;
    uint32_t combo_until;
    uint8_t multiplier;

    uint32_t run_start_ms;
    uint32_t level_start_ms;
    uint32_t played_ms;
    uint32_t level_time_ms;
    uint32_t longest_len;
    uint32_t bonuses;
    uint32_t run_foods;

    uint8_t slow;
    uint8_t multi;
    uint8_t ghost;
    uint16_t step_ms;
    uint32_t acc;
    uint32_t last_tick;
    uint32_t next_step_at;
    uint32_t hazard_tick;

    uint8_t alive;
    uint8_t endless;
    uint8_t practice;
    dead_t dead_why;
    uint8_t winfx;

    uint8_t occ[120]; /* 0 free, 1 snake, 2 wall, 3 hazard, 4 food, 5 power */
    particle_t particles[PARTICLE_MAX];
} serpent_game_t;

static serpent_game_t s_g;
static arc_state_t s_arc = ARC_OFF;
static uint32_t s_state_t0 = 0, s_last_render = 0, s_cd_step = 99;
static uint32_t s_fx_t0 = 0, s_over_t0 = 0;
static uint32_t s_hiscore = 0;
static uint8_t s_best_level = 1;
static uint8_t s_sel_level = 1;
static uint8_t s_rumble_mode = 2; /* V2 starts FULL */
static uint8_t s_inited = 0;
static uint32_t s_back_t0 = 0;
static uint8_t s_pad_lost = 0;
static uint8_t s_new_hiscore = 0;
static uint8_t s_saved_brightness = 50;
static uint8_t s_brightness_dirty = 0;
static uint8_t s_last_dead = DEAD_NONE;
static uint32_t s_total_games = 0, s_total_foods = 0, s_total_clears = 0;
static uint32_t s_all_longest = 0, s_all_max_combo = 0;
static uint32_t s_tick_now = 0;

/* TFT mirror: populated by the same task that renders the matrix. */
static uint8_t s_mirror[120][3];
static uint32_t s_mdirt[4];

/* ----------------------------- palette / math ---------------------------- */
static const rgb_t PAL[] = {
    {0, 140, 210}, {0, 190, 90}, {0, 220, 160}, {120, 60, 220},
    {220, 60, 180}, {240, 55, 75}, {255, 120, 30}, {255, 205, 35},
    {65, 175, 255}, {130, 255, 245}, {190, 90, 255}, {255, 80, 150}
};
static const rgb_t RAINBOW[] = {
    {220,0,0},{255,90,0},{255,220,0},{0,220,70},
    {0,170,255},{90,60,255},{220,0,220}
};

static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static uint8_t tri_wave(uint32_t now, uint32_t period)
{
    if (!period) return 255;
    uint32_t p = now % period;
    uint32_t h = period / 2u;
    if (h == 0) return 255;
    return (uint8_t)(p < h ? (p * 255u / h) : ((period - p) * 255u / h));
}

static rgb_t scale_rgb(rgb_t c, uint8_t pct)
{
    rgb_t o = {
        (uint8_t)((uint16_t)c.r * pct / 255u),
        (uint8_t)((uint16_t)c.g * pct / 255u),
        (uint8_t)((uint16_t)c.b * pct / 255u)
    };
    return o;
}

static rgb_t blend_rgb(rgb_t a, rgb_t b, uint8_t t)
{
    rgb_t o = {
        (uint8_t)(a.r + ((int)b.r - a.r) * t / 255),
        (uint8_t)(a.g + ((int)b.g - a.g) * t / 255),
        (uint8_t)(a.b + ((int)b.b - a.b) * t / 255)
    };
    return o;
}

static rgb_t hue6(uint16_t h, uint8_t v)
{
    /* 0..1535 six-sector integer HSV with full saturation. */
    h %= 1536u;
    uint8_t x = (uint8_t)((h % 256u) * v / 255u);
    uint8_t q = (uint8_t)(v - x);
    uint8_t r=0,gv=0,b=0;
    switch (h / 256u) {
        case 0: r=v; gv=x; break;
        case 1: r=q; gv=v; break;
        case 2: gv=v; b=x; break;
        case 3: gv=q; b=v; break;
        case 4: r=x; b=v; break;
        default: r=v; b=q; break;
    }
    return (rgb_t){r,gv,b};
}

static uint8_t level_band(uint8_t lv)
{
    if (lv <= 4) return 0;
    if (lv <= 8) return 3;
    if (lv <= 12) return 5;
    if (lv <= 16) return 8;
    if (lv <= 20) return 10;
    return 0;
}

/* ------------------------------- matrix --------------------------------- */
static void mirror_dirty(int idx)
{
    if (idx >= 0 && idx < 120) s_mdirt[idx >> 5] |= (1u << (idx & 31));
}

static void snk_px(int x, int y, rgb_t c)
{
    int fw = matrix_field_w(), fh = matrix_field_h();
    if (x < 0 || y < 0 || x >= fw || y >= fh) return;
    int idx = y * fw + x;
    s_mirror[idx][0] = c.r;
    s_mirror[idx][1] = c.g;
    s_mirror[idx][2] = c.b;
    mirror_dirty(idx);
    matrix_set_xy_rgb(x, y, c);
}

static void snk_clear(void)
{
    for (int i = 0; i < 120; i++) {
        s_mirror[i][0] = 0;
        s_mirror[i][1] = 0;
        s_mirror[i][2] = 0;
        mirror_dirty(i);
    }
    matrix_clear();
}

/* ----------------------------- sound / rumble ---------------------------- */
typedef struct { uint8_t l, r, sev; uint16_t ms; } rb_evt_t;
static rb_evt_t s_rbq[12];
static uint8_t s_rbq_n = 0;
static uint8_t s_rb_running = 0;
static uint32_t s_rb_until = 0;
static uint32_t s_rb_last_write = 0;

enum { RB_TICK, RB_EAT, RB_POWER, RB_LEVEL, RB_DANGER, RB_HI };

static void game_sound(const char *name, uint16_t fallback)
{
    if (!snd_play(name)) car_sfx_blip(fallback);
}

static void rb_raw(uint8_t l, uint8_t r, uint16_t ms, uint32_t now)
{
    if (s_rumble_mode == 0) return;
    if (s_rumble_mode == 1) { l /= 2; r /= 2; }
    if (l < 18 && r < 18) return;
    if (now - s_rb_last_write < 20u) return;
    s_rb_last_write = now;
    xbox360_rumble(0, l, r);
    s_rb_running = 1;
    s_rb_until = now + ms;
}

static void rb_push(uint8_t l, uint8_t r, uint16_t ms, uint8_t sev)
{
    if (s_rbq_n >= 12) {
        if (sev <= s_rbq[s_rbq_n - 1].sev) return;
        s_rbq_n = 0;
    }
    s_rbq[s_rbq_n++] = (rb_evt_t){l,r,sev,ms};
}

static void rb_food(void)
{
    rb_push(0, 165, 55, RB_EAT);
}
static void rb_combo(uint8_t combo)
{
    uint8_t s = (uint8_t)(110 + (combo > 6 ? 90 : combo * 20));
    rb_push((combo & 1) ? s : 0, (combo & 1) ? 0 : s, 55, RB_EAT);
}
static void rb_power(void)
{
    rb_push(60, 210, 60, RB_POWER);
    rb_push(180, 70, 60, RB_POWER);
    rb_push(255, 255, 90, RB_POWER);
}
static void rb_level(void)
{
    rb_push(60, 80, 80, RB_LEVEL);
    rb_push(110, 130, 90, RB_LEVEL);
    rb_push(180, 180, 100, RB_LEVEL);
    rb_push(255, 230, 130, RB_LEVEL);
}
static void rb_death(void)
{
    rb_push(255,255,210,RB_DANGER);
    rb_push(190,150,160,RB_DANGER);
    rb_push(90,55,120,RB_DANGER);
}
static void rb_hiscore(void)
{
    for (uint8_t i=0;i<6;i++)
        rb_push((i&1)?235:20,(i&1)?20:235,70,RB_HI);
}
static void rb_tick(void) { rb_push(0,100,28,RB_TICK); }

static void rumble_tick(uint32_t now)
{
    if (s_rb_running && (int32_t)(now - s_rb_until) >= 0) {
        s_rb_running = 0;
        xbox360_rumble(0,0,0);
    }
    if (!s_rb_running && s_rbq_n) {
        if (now - s_rb_last_write < 20u) return;
        rb_evt_t e = s_rbq[0];
        for (uint8_t i=1;i<s_rbq_n;i++) s_rbq[i-1] = s_rbq[i];
        s_rbq_n--;
        rb_raw(e.l,e.r,e.ms,now);
    }
}

static void rumble_stop_all(void)
{
    s_rbq_n = 0;
    s_rb_running = 0;
    s_rb_until = 0;
    xbox360_rumble(0,0,0);
}

/* ---------------------------- particle system ---------------------------- */
static void particles_clear(void)
{
    memset(s_g.particles, 0, sizeof(s_g.particles));
}

static void particle_burst(int x, int y, uint8_t type, uint32_t now)
{
    static const int8_t vx[8] = {1,-1,0,0,1,-1,1,-1};
    static const int8_t vy[8] = {0,0,1,-1,1,1,-1,-1};
    rgb_t base;
    switch (type) {
        case 1: base = (rgb_t){255,210,50}; break;    /* gold */
        case 2: base = (rgb_t){80,220,255}; break;    /* shield */
        case 3: base = (rgb_t){255,80,180}; break;    /* prism */
        case 4: base = (rgb_t){220,40,50}; break;     /* crash */
        case 5: base = (rgb_t){140,80,255}; break;    /* power */
        default: base = (rgb_t){100,255,120}; break;
    }
    for (int i=0;i<8;i++) {
        int slot = -1;
        for (int j=0;j<PARTICLE_MAX;j++) {
            if (!s_g.particles[j].active) { slot=j; break; }
        }
        if (slot < 0) break;
        particle_t *p = &s_g.particles[slot];
        p->x=(int8_t)x; p->y=(int8_t)y;
        p->vx=vx[i]; p->vy=vy[i];
        p->r=base.r; p->g=base.g; p->b=base.b;
        p->age_ms=(uint16_t)(i*18u);
        p->life_ms=(uint16_t)(320u + (i&3)*55u);
        p->active=1;
    }
    (void)now;
}

static void particles_tick(uint32_t dt)
{
    for (int i=0;i<PARTICLE_MAX;i++) {
        particle_t *p=&s_g.particles[i];
        if (!p->active) continue;
        p->age_ms=(uint16_t)(p->age_ms + (dt > 1000u ? 1000u : dt));
        if (p->age_ms >= p->life_ms) { p->active=0; continue; }
        if ((p->age_ms / 90u) != ((p->age_ms-dt) / 90u)) {
            p->x += p->vx;
            p->y += p->vy;
        }
    }
}

/* ------------------------------- occupancy -------------------------------- */
static bool wall_at(int x, int y, const sp_level_t *L)
{
    if (x<0 || y<0 || x>=FW || y>=FH) return true;
    if (matrix_get_orient() == MATRIX_ORIENT_PORTRAIT)
        return ((s_prows[y] >> x) & 1u) != 0;
    return ((L->block_rows[y] >> x) & 1u) != 0;
}

static void rebuild_occ(const sp_level_t *L)
{
    for (int i=0;i<120;i++) s_g.occ[i]=0;
    for (int y=0;y<FH;y++)
        for (int x=0;x<FW;x++)
            if (wall_at(x,y,L) || (!L->wrap && (x==0 || y==0 || x==FW-1 || y==FH-1)))
                s_g.occ[y*FW+x]=2;
    for (uint8_t i=0;i<s_g.length;i++) {
        int x=s_g.body[i].x,y=s_g.body[i].y;
        if (x>=0 && y>=0 && x<FW && y<FH) s_g.occ[y*FW+x]=1;
    }
    for (uint8_t i=0;i<s_g.mine_count;i++) {
        int x=s_g.mines[i].x,y=s_g.mines[i].y;
        if (x>=0 && y>=0 && x<FW && y<FH) s_g.occ[y*FW+x]=3;
    }
    for (uint8_t i=0;i<HAZARD_MAX;i++) if (s_g.hz[i].active) {
        int x=s_g.hz[i].p.x,y=s_g.hz[i].p.y;
        if (x>=0 && y>=0 && x<FW && y<FH) s_g.occ[y*FW+x]=3;
    }
    if (s_g.food.x>=0 && s_g.food.y>=0 && s_g.food.x<FW && s_g.food.y<FH)
        s_g.occ[s_g.food.y*FW+s_g.food.x]=4;
    if (s_g.power_kind != POWER_NONE && s_g.power.x>=0 && s_g.power.y>=0 &&
        s_g.power.x<FW && s_g.power.y<FH)
        s_g.occ[s_g.power.y*FW+s_g.power.x]=5;
}

static bool reachable_target(int tx, int ty, const sp_level_t *L)
{
    if (tx<0 || ty<0 || tx>=FW || ty>=FH) return false;
    static uint8_t seen[120];
    static uint8_t q[120];
    memset(seen,0,sizeof(seen));
    int sx=s_g.body[0].x, sy=s_g.body[0].y;
    if (sx<0 || sy<0 || sx>=FW || sy>=FH) return false;
    int qh=0,qt=0,si=sy*FW+sx;
    q[qt++]=(uint8_t)si; seen[si]=1;
    static const int8_t dx[4]={1,-1,0,0},dy[4]={0,0,1,-1};
    while(qh<qt) {
        int ci=q[qh++], cx=ci%FW, cy=ci/FW;
        if(cx==tx && cy==ty) return true;
        for(int d=0;d<4;d++) {
            int nx=cx+dx[d], ny=cy+dy[d];
            if(L->wrap) {
                nx=(nx+FW)%FW; ny=(ny+FH)%FH;
            } else if(nx<0 || ny<0 || nx>=FW || ny>=FH) continue;
            int ni=ny*FW+nx;
            if(seen[ni]) continue;
            if(s_g.occ[ni]==1 || s_g.occ[ni]==2 || s_g.occ[ni]==3) continue;
            seen[ni]=1; q[qt++]=(uint8_t)ni;
        }
    }
    return false;
}

static int manhattan(int x1,int y1,int x2,int y2)
{
    int dx=x1-x2, dy=y1-y2;
    if(dx<0) dx=-dx;
    if(dy<0) dy=-dy;
    return dx+dy;
}

static void clear_food(void)
{
    s_g.food.x=-1; s_g.food.y=-1; s_g.food_kind=FOOD_NORMAL; s_g.food_deadline=0;
}

static void spawn_food(void)
{
    const sp_level_t *L=sp_level(s_g.level);
    clear_food();
    rebuild_occ(L);
    uint8_t special_rate=L->special_rate;
    for(int tries=0;tries<80;tries++) {
        int x=(int)(esp_random()%FW), y=(int)(esp_random()%FH);
        if(s_g.occ[y*FW+x]) continue;
        if(manhattan(x,y,s_g.body[0].x,s_g.body[0].y)<2) continue;
        if(!reachable_target(x,y,L)) continue;
        s_g.food.x=(int8_t)x; s_g.food.y=(int8_t)y;
        uint32_t roll=esp_random()%100u;
        if(roll<special_rate) {
            s_g.food_kind=(roll&1u)?FOOD_GOLD:FOOD_PRISM;
            s_g.food_deadline=s_tick_now + (s_g.food_kind==FOOD_GOLD ? 2800u : 3400u);
        } else {
            s_g.food_kind=FOOD_NORMAL;
            s_g.food_deadline=0;
        }
        s_g.food_t0=s_tick_now;
        rebuild_occ(L);
        return;
    }
    /* Full board fallback: first available cell, never leave invalid target. */
    for(int y=0;y<FH;y++) for(int x=0;x<FW;x++) {
        if(!s_g.occ[y*FW+x]) {
            s_g.food.x=(int8_t)x; s_g.food.y=(int8_t)y;
            s_g.food_kind=FOOD_NORMAL; s_g.food_deadline=0; s_g.food_t0=s_tick_now;
            rebuild_occ(L); return;
        }
    }
}

static bool cell_free_for_power(int x,int y)
{
    if(x<0||y<0||x>=FW||y>=FH) return false;
    return s_g.occ[y*FW+x]==0;
}

static void spawn_power(void)
{
    if(s_g.power_kind!=POWER_NONE) return;
    const sp_level_t *L=sp_level(s_g.level);
    for(int tries=0;tries<60;tries++) {
        int x=(int)(esp_random()%FW), y=(int)(esp_random()%FH);
        if(!cell_free_for_power(x,y)) continue;
        if(manhattan(x,y,s_g.body[0].x,s_g.body[0].y)<3) continue;
        if(!reachable_target(x,y,L)) continue;
        s_g.power.x=(int8_t)x; s_g.power.y=(int8_t)y;
        uint8_t r=(uint8_t)(esp_random()%4u);
        s_g.power_kind=(power_kind_t)(POWER_SHIELD+r);
        /* Power duration starts when collected; this is an optional pickup, so no expiry here. */
        rebuild_occ(L); return;
    }
}

static void add_mine(void)
{
    const sp_level_t *L=sp_level(s_g.level);
    if(s_g.mine_count>=6) return;
    rebuild_occ(L);
    for(int t=0;t<60;t++) {
        int x=(int)(esp_random()%FW), y=(int)(esp_random()%FH);
        if(s_g.occ[y*FW+x]) continue;
        if(y>=s_corr_lo && y<=s_corr_hi) continue;
        if(manhattan(x,y,s_g.body[0].x,s_g.body[0].y)<4) continue;
        s_g.mines[s_g.mine_count].x=(int8_t)x;
        s_g.mines[s_g.mine_count].y=(int8_t)y;
        s_g.mine_count++;
        rebuild_occ(L);
        if(reachable_target(s_g.food.x,s_g.food.y,L)) return;
        s_g.mine_count--; rebuild_occ(L);
    }
}

/* -------------------------- hazard movement ----------------------------- */
static bool hazard_at(int x,int y)
{
    for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active)
        if(s_g.hz[i].p.x==x && s_g.hz[i].p.y==y) return true;
    for(uint8_t i=0;i<s_g.mine_count;i++)
        if(s_g.mines[i].x==x && s_g.mines[i].y==y) return true;
    return false;
}

static void spawn_hazards(const sp_level_t *L)
{
    memset(s_g.hz,0,sizeof(s_g.hz));
    uint8_t count=0;
    if(L->patrol) count=1;
    if(L->hazard_style>=2) count=2;
    for(uint8_t i=0;i<count;i++) {
        int x,y;
        bool ok=false;
        for(int t=0;t<80;t++) {
            x=(int)(esp_random()%FW);
            y=(int)(esp_random()%FH);
            if(!s_g.occ[y*FW+x] && manhattan(x,y,s_g.body[0].x,s_g.body[0].y)>=5) { ok=true; break; }
        }
        if(!ok) continue;
        s_g.hz[i].p.x=(int8_t)x; s_g.hz[i].p.y=(int8_t)y;
        s_g.hz[i].dx=(i&1)?-1:1; s_g.hz[i].dy=0;
        s_g.hz[i].active=1;
        s_g.hz[i].kind=(L->hazard_style>=2 && i==1)?2:1;
        s_g.hz[i].period=(L->hazard_style>=3)?1:2;
        s_g.hz[i].phase=i;
    }
}

static bool valid_hazard_destination(int x,int y,const sp_level_t *L)
{
    if(L->wrap) {
        x=(x+FW)%FW; y=(y+FH)%FH;
    } else if(x<0||y<0||x>=FW||y>=FH) return false;
    if(wall_at(x,y,L)) return false;
    for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active &&
        s_g.hz[i].p.x==x && s_g.hz[i].p.y==y) return false;
    for(uint8_t i=0;i<s_g.mine_count;i++) if(s_g.mines[i].x==x && s_g.mines[i].y==y) return false;
    return true;
}

static void move_hazards(const sp_level_t *L)
{
    s_g.hazard_tick++;
    for(uint8_t i=0;i<HAZARD_MAX;i++) {
        hazard_t *h=&s_g.hz[i];
        if(!h->active || (s_g.hazard_tick + h->phase) % h->period) continue;
        int nx=h->p.x+h->dx, ny=h->p.y+h->dy;
        if(h->kind==2) {
            /* Predictive hunter: move only one cell, prefer axis that closes on head. */
            int hx=s_g.body[0].x, hy=s_g.body[0].y;
            int bestx=h->p.x, besty=h->p.y, best=9999;
            const int8_t opts[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
            for(int k=0;k<4;k++) {
                int tx=h->p.x+opts[k][0], ty=h->p.y+opts[k][1];
                if(L->wrap){tx=(tx+FW)%FW;ty=(ty+FH)%FH;}
                if(!valid_hazard_destination(tx,ty,L)) continue;
                int d=manhattan(tx,ty,hx,hy);
                if(d<best){best=d;bestx=tx;besty=ty;}
            }
            nx=bestx; ny=besty;
        }
        if(L->wrap) { nx=(nx+FW)%FW; ny=(ny+FH)%FH; }
        if(valid_hazard_destination(nx,ny,L)) {
            h->p.x=(int8_t)nx; h->p.y=(int8_t)ny;
        } else {
            h->dx=(int8_t)-h->dx;
            h->dy=(int8_t)-h->dy;
        }
    }
    rebuild_occ(L);
}

/* ------------------------------- setup ---------------------------------- */
static void transpose_map(const sp_level_t *L)
{
    memset(s_prows,0,sizeof(s_prows));
    if(matrix_get_orient()!=MATRIX_ORIENT_PORTRAIT) return;
    for(int py=0;py<12;py++) {
        uint16_t m=0;
        for(int px=0;px<10;px++)
            if((L->block_rows[9-px]>>py)&1u) m|=(uint16_t)(1u<<px);
        s_prows[py]=m;
    }
}

static bool start_position_clear(int x,int y,uint8_t len,direction_t d,const sp_level_t *L)
{
    int dx=0,dy=0;
    if(d==DIR_LEFT)dx=1; else if(d==DIR_RIGHT)dx=-1;
    else if(d==DIR_UP)dy=1; else dy=-1;
    for(uint8_t i=0;i<len;i++) {
        int px=x+dx*i, py=y+dy*i;
        if(px<0||py<0||px>=FW||py>=FH||wall_at(px,py,L) || (!L->wrap && (px==0||py==0||px==FW-1||py==FH-1))) return false;
    }
    return true;
}

static void choose_start(const sp_level_t *L,uint8_t len)
{
    static const cell_t candidates[]={{6,5},{5,5},{6,4},{5,6},{3,5},{8,5},{6,3},{6,6},{4,4}};
    for(size_t i=0;i<sizeof(candidates)/sizeof(candidates[0]);i++) {
        int x=candidates[i].x,y=candidates[i].y;
        if(start_position_clear(x,y,len,DIR_RIGHT,L)) {
            for(uint8_t k=0;k<len;k++){s_g.body[k].x=(int8_t)(x-k);s_g.body[k].y=(int8_t)y;}
            s_g.dir=s_g.last_applied=DIR_RIGHT;
            s_corr_lo=(uint8_t)(y>1?y-1:0); s_corr_hi=(uint8_t)(y+1<FH?y+1:FH-1);
            return;
        }
        if(start_position_clear(x,y,len,DIR_DOWN,L)) {
            for(uint8_t k=0;k<len;k++){s_g.body[k].x=(int8_t)x;s_g.body[k].y=(int8_t)(y+k);}
            s_g.dir=s_g.last_applied=DIR_DOWN;
            s_corr_lo=(uint8_t)(x>1?x-1:0); s_corr_hi=(uint8_t)(x+1<FH?x+1:FH-1);
            return;
        }
    }
    for(uint8_t k=0;k<len;k++){s_g.body[k].x=(int8_t)(6-k);s_g.body[k].y=5;}
    s_g.dir=s_g.last_applied=DIR_RIGHT;
}

static void start_level(uint8_t lv,uint32_t now,uint8_t preserve_run)
{
    s_tick_now=now;
    const sp_level_t *L=sp_level(lv);
    field_refresh();
    transpose_map(L);
    memset(&s_g,0,sizeof(s_g));
    s_g.level=lv;
    s_g.length=(uint8_t)(3u + lv/3u);
    if(s_g.length>10) s_g.length=10;
    s_g.lives=preserve_run ? s_g.lives : 3; /* caller copies preserved values back when needed */
    s_g.step_ms=L->step_ms;
    if(preserve_run) {
        /* preserve_run only means the caller will immediately restore run fields. */
    }
    choose_start(L,s_g.length);
    s_g.food.x=-1; s_g.food.y=-1;
    s_g.power_kind=POWER_NONE;
    s_g.alive=1;
    s_g.level_start_ms=now;
    s_g.last_tick=now;
    s_g.next_step_at=now+FIRST_MOVE_DELAY;
    s_g.dead_why=DEAD_NONE;
    s_g.level_special_target=(uint8_t)(L->bonus_on ? 1 : 0);
    particles_clear();
    rebuild_occ(L);
    spawn_food();
    for(uint8_t i=0;i<L->mines_start;i++) add_mine();
    rebuild_occ(L);
    spawn_hazards(L);
    rebuild_occ(L);
    s_g.level_score=s_g.score;
}

static void begin_run(uint8_t lv,uint32_t now,uint8_t practice)
{
    memset(&s_g,0,sizeof(s_g));
    s_g.level=lv;
    s_g.lives=3;
    s_g.practice=practice;
    s_g.run_start_ms=now;
    s_g.longest_len=0;
    s_g.level_start_ms=now;
    s_g.multiplier=1;
    start_level(lv,now,0);
    s_g.score=0;
    s_g.lives=3;
    s_g.practice=practice;
    s_g.run_start_ms=now;
    s_g.played_ms=0;
    s_g.run_foods=0;
    s_g.bonuses=0;
    s_g.max_combo=0;
}

/* ------------------------------ scoring ---------------------------------- */
static uint8_t combo_mult(void)
{
    uint8_t m=(uint8_t)(1 + s_g.combo/3u);
    if(m>5)m=5;
    return m;
}

static uint32_t nearest_hazard_distance(int x,int y)
{
    uint32_t best=999;
    for(uint8_t i=0;i<s_g.mine_count;i++) {
        uint32_t d=(uint32_t)manhattan(x,y,s_g.mines[i].x,s_g.mines[i].y);
        if(d<best)best=d;
    }
    for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active) {
        uint32_t d=(uint32_t)manhattan(x,y,s_g.hz[i].p.x,s_g.hz[i].p.y);
        if(d<best)best=d;
    }
    return best;
}

static void apply_food_score(food_kind_t kind,uint32_t now)
{
    if((int32_t)(now-s_g.combo_until)>=0) s_g.combo=0;
    if(s_g.combo<250) s_g.combo++;
    if(s_g.combo>s_g.max_combo) s_g.max_combo=s_g.combo;
    if(s_g.max_combo>s_all_max_combo) s_all_max_combo=s_g.max_combo;
    s_g.combo_until=now+COMBO_WINDOW_MS;
    uint8_t cm=combo_mult();
    uint32_t base=10u*s_g.level;
    if(kind==FOOD_GOLD) base=50u*s_g.level;
    else if(kind==FOOD_PRISM) base=100u*s_g.level;
    uint32_t risk=(nearest_hazard_distance(s_g.body[0].x,s_g.body[0].y)<=2u)?(10u*s_g.level):0u;
    uint32_t speed_bonus=(s_g.step_ms<130u)?(5u*s_g.level):0u;
    uint32_t mult=(uint32_t)cm*(s_g.multi?2u:1u);
    uint32_t add=base*mult + risk + speed_bonus;
    s_g.score+=add;
    s_g.food_in_level++;
    s_g.run_foods++;
    s_total_foods++;
    s_g.bonuses += risk + speed_bonus;
    if(kind!=FOOD_NORMAL) s_g.special_collected++;
    if(s_g.multi) rb_combo((uint8_t)(s_g.combo+2)); else rb_food();
    game_sound(kind==FOOD_NORMAL?"score_tick":"score_reveal", kind==FOOD_NORMAL?900:1200);
    particle_burst(s_g.body[0].x,s_g.body[0].y,kind==FOOD_GOLD?1:(kind==FOOD_PRISM?3:0),now);
    if(s_g.combo>=3 && (s_g.combo==3 || (s_g.combo%3)==0)) {
        game_sound("ui_toggle_on",1100);
        rb_combo(s_g.combo);
    }
}

static void collect_power(power_kind_t p,uint32_t now)
{
    s_g.power_kind=POWER_NONE;
    s_g.active_power=p;
    switch(p) {
        case POWER_SHIELD: s_g.shield=1; s_g.power_until=now+POWER_SHIELD_MS; break;
        case POWER_SLOW: s_g.slow=1; s_g.power_until=now+POWER_SLOW_MS; break;
        case POWER_MULTI: s_g.multi=1; s_g.power_until=now+POWER_MULTI_MS; break;
        case POWER_GHOST: s_g.ghost=1; s_g.power_until=now+POWER_GHOST_MS; break;
        default: break;
    }
    s_g.bonuses+=25u*s_g.level;
    game_sound(p==POWER_SHIELD?"ui_toggle_on": "score_reveal", 1300);
    rb_power();
    particle_burst(s_g.body[0].x,s_g.body[0].y,5,now);
}

static void update_power_expiry(uint32_t now)
{
    if(s_g.power_until && (int32_t)(now-s_g.power_until)>=0) {
        s_g.shield=0; s_g.slow=0; s_g.multi=0; s_g.ghost=0; s_g.active_power=POWER_NONE; s_g.power_until=0;
    }
}

static void level_complete(uint32_t now)
{
    uint32_t elapsed=now-s_g.level_start_ms;
    uint32_t secs=elapsed/1000u;
    uint32_t time_bonus=0;
    if(secs<55u) time_bonus=(55u-secs)*10u*s_g.level;
    uint32_t life_bonus=(uint32_t)s_g.lives*50u*s_g.level;
    uint32_t combo_bonus=(uint32_t)s_g.max_combo*20u*s_g.level;
    uint32_t objective=(s_g.level_special_target && s_g.special_collected>=1)?100u*s_g.level:0u;
    s_g.score+=time_bonus+life_bonus+combo_bonus+objective;
    s_g.bonuses+=time_bonus+life_bonus+combo_bonus+objective;
    s_total_clears++;
    if(!s_g.practice && s_g.level >= s_best_level && s_g.level < 20)
        s_best_level=(uint8_t)(s_g.level+1);
    game_sound("score_reveal",1400);
    rb_level();
    s_fx_t0=now;
    s_arc=ARC_CLEAR;
    s_state_t0=now;
    s_g.winfx=(s_g.level>=20)?1:0;
}

/* ------------------------------- simulation ------------------------------ */
static bool consume_collision(int nowx,int nowy,dead_t why,uint32_t now)
{
    if(s_g.ghost && why!=DEAD_WALL) return false;
    if(s_g.shield) {
        s_g.shield=0;
        s_g.power_until=0;
        s_g.power_kind=POWER_NONE;
        s_g.active_power=POWER_NONE;
        particle_burst(nowx,nowy,2,now);
        rb_power();
        game_sound("ui_error",1200);
        return false;
    }
    s_g.alive=0;
    s_g.dead_why=why;
    s_last_dead=(uint8_t)why;
    s_fx_t0=now;
    rb_death();
    game_sound("impact_hard",350);
    particle_burst(nowx,nowy,4,now);
    return true;
}

static void do_step(uint32_t now)
{
    const sp_level_t *L=sp_level(s_g.level);
    if(s_g.queue_n) {
        s_g.dir=s_g.queue[0];
        if(s_g.queue_n>1) s_g.queue[0]=s_g.queue[1];
        s_g.queue_n--;
    }
    s_g.last_applied=s_g.dir;
    cell_t nh=s_g.body[0];
    if(s_g.dir==DIR_UP) nh.y--; else if(s_g.dir==DIR_DOWN) nh.y++;
    else if(s_g.dir==DIR_LEFT) nh.x--; else nh.x++;
    if(L->wrap) {
        nh.x=(int8_t)((nh.x+FW)%FW); nh.y=(int8_t)((nh.y+FH)%FH);
    } else if(nh.x<0||nh.y<0||nh.x>=FW||nh.y>=FH) {
        (void)consume_collision(nh.x,nh.y,DEAD_WALL,now);
        return;
    }
    if(wall_at(nh.x,nh.y,L) || (!L->wrap && (nh.x==0||nh.y==0||nh.x==FW-1||nh.y==FH-1))) {
        (void)consume_collision(nh.x,nh.y,DEAD_WALL,now);
        return;
    }
    int ni=nh.y*FW+nh.x;
    uint8_t c=s_g.occ[ni];
    if(c==3 && !s_g.ghost) {
        (void)consume_collision(nh.x,nh.y,DEAD_MINE,now);
        return;
    }

    bool eats=(c==4), gets_power=(c==5);
    if(c==1 && !s_g.ghost) {
        int ti=s_g.body[s_g.length-1].y*FW+s_g.body[s_g.length-1].x;
        /* Moving into the current tail is legal only when the tail advances. */
        if(ni!=ti) {
            (void)consume_collision(nh.x,nh.y,DEAD_SELF,now);
            return;
        }
    }

    if(eats || gets_power) {
        if(s_g.length<SERPENT_MAX_LEN) s_g.length++;
        for(int i=(int)s_g.length-1;i>0;i--) s_g.body[i]=s_g.body[i-1];
    } else {
        for(int i=(int)s_g.length-1;i>0;i--) s_g.body[i]=s_g.body[i-1];
    }
    s_g.body[0]=nh;
    if(s_g.length>s_g.longest_len) s_g.longest_len=s_g.length;
    if(s_g.longest_len>s_all_longest) s_all_longest=s_g.longest_len;

    if(eats) {
        food_kind_t fk=s_g.food_kind;
        apply_food_score(fk,now);
        clear_food();
        rebuild_occ(L);
        /* every sixth normal-food event has a chance to drop a power-up */
        if((s_g.run_foods%6u)==0u && L->bonus_on && (esp_random()%100u)<55u) spawn_power();
        spawn_food();
        if((s_g.food_in_level%4u)==0u && s_g.mine_count<L->mines_max) add_mine();
        if(!s_g.endless && s_g.food_in_level>=L->food_target) {
            level_complete(now); return;
        }
    } else if(gets_power) {
        power_kind_t pk=s_g.power_kind;
        collect_power(pk,now);
        s_g.special_collected++;
        s_g.power.x=-1; s_g.power.y=-1;
        rebuild_occ(L);
        spawn_food();
    }

    /* Move hazards after the player, then evaluate an intentional enemy approach. */
    move_hazards(L);
    for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active &&
        s_g.hz[i].p.x==s_g.body[0].x && s_g.hz[i].p.y==s_g.body[0].y) {
        if(consume_collision(s_g.body[0].x,s_g.body[0].y,DEAD_MINE,now)) return;
    }

    if(!s_g.endless && s_g.food_in_level%5u==0u && s_g.mine_count<L->mines_max) add_mine();
}

static uint16_t effective_step(void)
{
    uint32_t step=s_g.step_ms;
    if(s_g.endless) {
        uint32_t reduction=(s_g.endless_food/5u)*4u;
        step=(step>reduction)?step-reduction:ENDLESS_MIN_STEP_MS;
        if(step<ENDLESS_MIN_STEP_MS) step=ENDLESS_MIN_STEP_MS;
    }
    if(s_g.slow) step += step/2u;
    if(step>65000u) step=65000u;
    return (uint16_t)step;
}

/* ------------------------------- rendering ------------------------------ */
static void draw_digit(int x,int y,int d,rgb_t c)
{
    static const uint8_t DIG[10][5]={{7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},
                                     {5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,2,2},
                                     {7,5,7,5,7},{7,5,7,1,7}};
    if(d<0||d>9)return;
    for(int r=0;r<5;r++)for(int col=0;col<3;col++)
        if(DIG[d][r]&(1u<<(2-col)))snk_px(x+col,y+r,c);
}

static void draw_number_center(uint8_t n,rgb_t c)
{
    if(n>=10) { draw_digit((FW-7)/2,2,n/10,c); draw_digit((FW-7)/2+4,2,n%10,c); }
    else draw_digit((FW-3)/2,2,n,c);
}

static void render_menu(uint32_t now)
{
    snk_clear();
    rgb_t a=PAL[level_band(s_sel_level)%12];
    for(int x=1;x<FW-1;x++) {
        uint8_t p=(uint8_t)(80u + tri_wave(now+((uint32_t)x*90u),900u)/2u);
        snk_px(x,1,scale_rgb(a,p));
    }
    /* compact snake logo */
    snk_px(2,4,scale_rgb(COL_GREEN,220));
    snk_px(3,4,scale_rgb(COL_GREEN,210));
    snk_px(4,4,scale_rgb(COL_GREEN,200));
    snk_px(4,5,scale_rgb(COL_GREEN,190));
    snk_px(5,5,scale_rgb(COL_HEAD,230));
    snk_px(FW-3,4,scale_rgb(COL_GOLD,tri_wave(now,700)+60));
    draw_number_center(s_sel_level,(rgb_t){255,210,55});
    for(int i=0;i<3;i++) snk_px(2+i*2,FH-2,(i<s_g.lives||!s_g.lives)?(rgb_t){230,50,55}:(rgb_t){60,20,20});
    matrix_show();
}

static void render_countdown(uint32_t now)
{
    uint32_t el=now-s_state_t0, step=el/650u;
    if(step!=s_cd_step) { s_cd_step=(uint8_t)step; rb_tick(); game_sound(step<3?"ui_nav":"score_reveal",step<3?500:1000); }
    snk_clear();
    if(step<3) draw_number_center((uint8_t)(3-step),(rgb_t){0,210,255});
    else {
        uint8_t p=(uint8_t)(80u+tri_wave(now,500u)/2u);
        for(int x=0;x<FW;x++){snk_px(x,0,scale_rgb((rgb_t){0,220,100},p));snk_px(x,FH-1,scale_rgb((rgb_t){0,220,100},p));}
    }
    matrix_show();
}

static void render_intro(void)
{
    snk_clear();
    draw_number_center(s_g.level,(rgb_t){255,205,35});
    for(int i=0;i<3;i++) snk_px(3+i*2,FH-2,i<s_g.lives?(rgb_t){230,40,55}:(rgb_t){50,15,18});
    if(s_g.endless) snk_px(FW-2,FH-2,(rgb_t){180,70,255});
    matrix_show();
}

static void render_background(uint32_t now)
{
    uint8_t band=level_band(s_g.level);
    rgb_t c0=PAL[band%12], c1=PAL[(band+2)%12];
    /* low-power ambient field: animated sparse stars, never full-white. */
    for(int i=0;i<18;i++) {
        int x=(i*7+((int)(now/220u)))%FW;
        int y=(i*5+((int)(now/370u)))%FH;
        uint8_t pulse=(uint8_t)(18u+tri_wave(now+(uint32_t)i*53u,1200u)/8u);
        snk_px(x,y,blend_rgb(scale_rgb(c0,pulse),scale_rgb(c1,pulse/2u),90));
    }
}

static void render_play(uint32_t now)
{
    const sp_level_t *L=sp_level(s_g.level);
    snk_clear();
    render_background(now);
    bool port=matrix_get_orient()==MATRIX_ORIENT_PORTRAIT;
    rgb_t wall=scale_rgb(PAL[level_band(s_g.level)%12],50);
    rgb_t edge=scale_rgb(PAL[(level_band(s_g.level)+1)%12],70);
    for(int y=0;y<FH;y++) for(int x=0;x<FW;x++) {
        bool block=port?(((s_prows[y]>>x)&1u)!=0):(((L->block_rows[y]>>x)&1u)!=0);
        if(block) snk_px(x,y,wall);
    }
    if(!L->wrap) {
        for(int x=0;x<FW;x++){snk_px(x,0,edge);snk_px(x,FH-1,edge);}
        for(int y=0;y<FH;y++){snk_px(0,y,edge);snk_px(FW-1,y,edge);}
    }
    /* Hazard glow. */
    for(uint8_t i=0;i<s_g.mine_count;i++) {
        uint8_t p=(uint8_t)(85u+tri_wave(now+i*130u,520u)/2u);
        snk_px(s_g.mines[i].x,s_g.mines[i].y,scale_rgb((rgb_t){230,35,150},p));
    }
    for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active) {
        uint8_t p=(uint8_t)(100u+tri_wave(now+i*180u,430u)/2u);
        rgb_t hc=s_g.hz[i].kind==2?(rgb_t){255,80,40}:(rgb_t){150,65,255};
        snk_px(s_g.hz[i].p.x,s_g.hz[i].p.y,scale_rgb(hc,p));
    }
    /* Power-up. */
    if(s_g.power_kind!=POWER_NONE) {
        rgb_t pc=(s_g.power_kind==POWER_SHIELD)?(rgb_t){40,220,255}:
                 (s_g.power_kind==POWER_SLOW)?(rgb_t){80,140,255}:
                 (s_g.power_kind==POWER_MULTI)?(rgb_t){255,190,30}:(rgb_t){190,70,255};
        uint8_t p=(uint8_t)(80u+tri_wave(now,420u)/2u);
        snk_px(s_g.power.x,s_g.power.y,scale_rgb(pc,p));
        if(s_g.power.x+1<FW && ((now/160u)&1u)) snk_px(s_g.power.x+1,s_g.power.y,scale_rgb(pc,p/2u));
    }
    /* Food. */
    if(s_g.food.x>=0 && s_g.food.y>=0) {
        uint32_t age=now-s_g.food_t0;
        if(s_g.food_deadline && (int32_t)(s_g.food_deadline-now)<=0) {
            /* Expire rare food and immediately replace it. */
            clear_food(); spawn_food();
        }
        uint8_t p=(uint8_t)(70u+tri_wave(now,420u)/2u);
        rgb_t fc;
        if(s_g.food_kind==FOOD_GOLD) fc=scale_rgb((rgb_t){255,210,40},p);
        else if(s_g.food_kind==FOOD_PRISM) fc=hue6((uint16_t)((now/4u)%1536u),p);
        else fc=scale_rgb(PAL[(level_band(s_g.level)+5)%12],p);
        if(age<160u) fc=scale_rgb(fc,(uint8_t)(40u+age*215u/160u));
        snk_px(s_g.food.x,s_g.food.y,fc);
        if(s_g.food_kind!=FOOD_NORMAL && ((now/120u)&1u)==0) {
            if(s_g.food.x>0) snk_px(s_g.food.x-1,s_g.food.y,scale_rgb(fc,50));
            if(s_g.food.x+1<FW) snk_px(s_g.food.x+1,s_g.food.y,scale_rgb(fc,50));
        }
    }
    /* Snake gradient with combo/power-up visual state. */
    rgb_t base0=PAL[level_band(s_g.level)%12], base1=PAL[(level_band(s_g.level)+2)%12];
    if(s_g.multi){base0=(rgb_t){255,200,50};base1=(rgb_t){255,90,180};}
    if(s_g.ghost){base0=(rgb_t){160,80,255};base1=(rgb_t){80,200,255};}
    for(int i=(int)s_g.length-1;i>=0;i--) {
        uint8_t t=(uint8_t)(i*255u/(s_g.length?s_g.length:1u));
        rgb_t c=blend_rgb(base0,base1,(uint8_t)(255u-t));
        uint8_t glow=(uint8_t)(150u+((s_g.length-i)*8u>90u?90u:(s_g.length-i)*8u));
        if(i==0) glow=(uint8_t)(210u+tri_wave(now,260u)/5u);
        c=scale_rgb(c,glow);
        if(s_g.ghost && ((now/90u+i)&1u)) c=scale_rgb(c,70);
        snk_px(s_g.body[i].x,s_g.body[i].y,c);
    }
    if(s_g.shield) {
        uint8_t p=(uint8_t)(80u+tri_wave(now,500u)/2u);
        snk_px(s_g.body[0].x,s_g.body[0].y,scale_rgb((rgb_t){40,220,255},p));
    }
    /* Combo pulse around head. */
    if(s_g.combo>=3 && ((now/180u)&1u)==0) {
        rgb_t cc=hue6((uint16_t)((s_g.combo*170u+now/5u)%1536u),170);
        int hx=s_g.body[0].x, hy=s_g.body[0].y;
        if(hx>0) snk_px(hx-1,hy,cc);
        if(hx+1<FW) snk_px(hx+1,hy,cc);
    }
    /* Particles layer. */
    for(int i=0;i<PARTICLE_MAX;i++) if(s_g.particles[i].active) {
        particle_t *p=&s_g.particles[i];
        uint8_t fade=(uint8_t)(255u-(uint32_t)p->age_ms*255u/p->life_ms);
        snk_px(p->x,p->y,(rgb_t){(uint8_t)((uint16_t)p->r*fade/255u),
                                  (uint8_t)((uint16_t)p->g*fade/255u),
                                  (uint8_t)((uint16_t)p->b*fade/255u)});
    }
    matrix_show();
}

static void render_dying(uint32_t now)
{
    uint32_t el=now-s_fx_t0;
    snk_clear();
    uint8_t p=(uint8_t)(240u-(el>1000u?240u:el*240u/1000u));
    if(((el/100u)&1u)==0) {
        for(uint8_t i=0;i<s_g.length;i++)
            snk_px(s_g.body[i].x,s_g.body[i].y,scale_rgb((rgb_t){255,35,45},p));
    }
    if(el<450u) {
        for(int y=0;y<FH;y++) for(int x=0;x<FW;x++)
            if(((x+y+(int)(el/80u))&1)==0) snk_px(x,y,scale_rgb((rgb_t){255,60,30},45));
    }
    matrix_show();
}

static void render_clear(uint32_t now)
{
    snk_clear();
    uint32_t el=now-s_fx_t0;
    if(s_g.winfx) {
        for(int y=0;y<FH;y++)for(int x=0;x<FW;x++)
            snk_px(x,y,RAINBOW[(x+y+(now/90u))%7]);
    } else {
        uint32_t rows=(el/55u)+1u;
        rgb_t c=PAL[level_band(s_g.level)%12];
        for(uint32_t y=0;y<rows && y<(uint32_t)FH;y++)
            for(int x=0;x<FW;x++) snk_px(x,(int)y,scale_rgb(c,(uint8_t)(90u+tri_wave(now+x*30u,700u)/2u)));
    }
    if(s_g.winfx && ((now/180u)&1u)==0) {
        for(int x=0;x<FW;x++){snk_px(x,0,(rgb_t){255,255,255});snk_px(x,FH-1,(rgb_t){255,255,255});}
    }
    matrix_show();
}

static void render_over(uint32_t now)
{
    snk_clear();
    uint32_t el=now-s_over_t0;
    uint8_t p=(uint8_t)(120u+tri_wave(now,480u)/2u);
    for(int x=0;x<FW;x++) {
        snk_px(x,0,scale_rgb((rgb_t){255,50,50},p));
        snk_px(x,FH-1,scale_rgb((rgb_t){255,160,30},p));
    }
    draw_number_center(s_g.level,(rgb_t){255,60,70});
    uint32_t bars=s_g.score/20u; if(bars>24u)bars=24u;
    for(uint32_t i=0;i<bars;i++) snk_px((int)(i%FW),(FH-2)-(int)(i/FW),(rgb_t){255,190,40});
    if(s_new_hiscore) {
        for(int x=0;x<FW;x++) {
            rgb_t rc=RAINBOW[(x+(int)(now/100u))%7];
            snk_px(x,0,rc); snk_px(x,FH-1,rc);
        }
    }
    (void)el;
    matrix_show();
}

/* ------------------------------- state ---------------------------------- */
static void save_stats(void)
{
    nvs_handle_t h;
    if(nvs_open("arcade",NVS_READWRITE,&h)!=ESP_OK) return;
    uint32_t best=s_best_level;
    if(nvs_set_u32(h,NVS_HI_KEY,s_hiscore)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_BEST_KEY,best)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_LONGEST_KEY,s_all_longest)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_COMBO_KEY,s_all_max_combo)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_GAMES_KEY,s_total_games)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_FOODS_KEY,s_total_foods)!=ESP_OK) { nvs_close(h); return; }
    if(nvs_set_u32(h,NVS_CLEAR_KEY,s_total_clears)!=ESP_OK) { nvs_close(h); return; }
    nvs_commit(h);
    nvs_close(h);
}

static void load_stats(void)
{
    nvs_handle_t h;
    if(nvs_open("arcade",NVS_READONLY,&h)!=ESP_OK) return;
    uint32_t v=0;
    if(nvs_get_u32(h,NVS_HI_KEY,&v)==ESP_OK) s_hiscore=v;
    else if(nvs_get_u32(h,"serp_hi2",&v)==ESP_OK) s_hiscore=v;
    if(nvs_get_u32(h,NVS_BEST_KEY,&v)==ESP_OK && v>=1 && v<=20) s_best_level=(uint8_t)v;
    else { uint8_t oldb=0; if(nvs_get_u8(h,"serp_lvl",&oldb)==ESP_OK && oldb>=1 && oldb<=10) s_best_level=oldb; }
    if(nvs_get_u32(h,NVS_LONGEST_KEY,&v)==ESP_OK) s_all_longest=v;
    if(nvs_get_u32(h,NVS_COMBO_KEY,&v)==ESP_OK) s_all_max_combo=v;
    if(nvs_get_u32(h,NVS_GAMES_KEY,&v)==ESP_OK) s_total_games=v;
    if(nvs_get_u32(h,NVS_FOODS_KEY,&v)==ESP_OK) s_total_foods=v;
    if(nvs_get_u32(h,NVS_CLEAR_KEY,&v)==ESP_OK) s_total_clears=v;
    uint8_t b=0;
    if(nvs_get_u8(h,NVS_RUMBLE_KEY,&b)==ESP_OK && b<=2) s_rumble_mode=b;
    nvs_close(h);
}

static void enter_menu(uint32_t now)
{
    s_arc=ARC_MENU;
    s_state_t0=now;
    if(s_sel_level<1 || s_sel_level>s_best_level) s_sel_level=1;
    s_saved_brightness=car_get_rgb_bright();
    led_matrix_acquire(OWNER_ARCADE);
    led_matrix_set_demo(false);
    led_matrix_set_dim(false);
    matrix_set_dim_pct(100);
    input_flush_context_switch();
    game_sound("ui_open_sheet",700);
    notif_push(NOTIF_RULES,"ARCADE: NEON SERPENT",now);
}

static void exit_full(uint32_t now)
{
    (void)now;
    s_arc=ARC_OFF;
    s_back_t0=0;
    rumble_stop_all();
    if(s_g.score>s_hiscore && !s_g.practice && s_g.score>0) {
        s_hiscore=s_g.score; s_new_hiscore=1;
    }
    save_stats();
    if(s_brightness_dirty) { s_brightness_dirty=0; car_settings_save(); }
    led_matrix_safe_off();
    led_matrix_release(OWNER_ARCADE);
    led_matrix_set_demo(true);
    matrix_set_brightness(s_saved_brightness);
    matrix_set_dim_pct(100);
    input_flush_context_switch();
    os_ctx_t *oc=car_os_ctx();
    if(oc && oc->state==OS_GAME_3) os_request_screen(oc,OS_GAMES_HUB,now);
}

static void game_over(uint32_t now)
{
    s_arc=ARC_OVER;
    s_over_t0=now;
    s_state_t0=now;
    s_new_hiscore=(!s_g.practice && s_g.score>s_hiscore && s_g.score>0)?1:0;
    if(s_new_hiscore) { s_hiscore=s_g.score; rb_hiscore(); game_sound("highscore_fanfare",1600); }
    else { rb_death(); game_sound("game_over",300); }
    save_stats();
    const char *why=s_g.dead_why==DEAD_WALL?"WALL":s_g.dead_why==DEAD_SELF?"SELF":"MINE";
    ESP_LOGI(TAG,"over score=%lu hi=%lu level=%u cause=%s combo=%u longest=%lu",
             (unsigned long)s_g.score,(unsigned long)s_hiscore,s_g.level,why,
             s_g.max_combo,(unsigned long)s_g.longest_len);
    notif_push(NOTIF_RULES,s_new_hiscore?"SERPENT NEW HIGH SCORE":"SERPENT GAME OVER",now);
}

/* ------------------------------- public --------------------------------- */
void serpent_arcade_init(void)
{
    if(s_inited) return;
    s_inited=1;
    load_stats();
    field_refresh();
    memset(s_mirror,0,sizeof(s_mirror));
    memset(s_mdirt,0,sizeof(s_mdirt));
    ESP_LOGI(TAG,"NEON SERPENT V2 init hi=%lu best=%u rumble=%u",
             (unsigned long)s_hiscore,s_best_level,s_rumble_mode);
}

bool serpent_arcade_active(void) { return s_arc!=ARC_OFF; }
uint32_t serpent_hiscore(void) { return s_hiscore; }
uint8_t serpent_rumble_mode(void) { return s_rumble_mode; }

void serpent_set_rumble_mode(uint8_t m)
{
    if(m>2)return;
    s_rumble_mode=m;
    nvs_handle_t h;
    if(nvs_open("arcade",NVS_READWRITE,&h)==ESP_OK) {
        nvs_set_u8(h,NVS_RUMBLE_KEY,m); nvs_commit(h); nvs_close(h);
    }
}

uint8_t serpent_arcade_state(void) { return (uint8_t)s_arc; }
bool serpent_suppress_oled(void)
{
    return s_arc==ARC_COUNTDOWN||s_arc==ARC_INTRO||s_arc==ARC_PLAY||
           s_arc==ARC_DYING||s_arc==ARC_RETRY||s_arc==ARC_CLEAR;
}

bool serpent_arcade_open(void)
{
    if(s_arc!=ARC_OFF)return true;
    const xbox360_pad_t *p=xbox360_pad(0);
    bool padok=p&&p->present&&xbox360_dongle_connected();
    if(!led_matrix_ready()||g.estop||os_game_active()||!os_parked()||!padok) {
        ESP_LOGW(TAG,"arcade blocked matrix=%d estop=%d game=%d parked=%d pad=%d",
                 led_matrix_ready(),g.estop,os_game_active(),os_parked(),padok);
        return false;
    }
    enter_menu((uint32_t)(esp_timer_get_time()/1000u));
    return true;
}

static void update_play_time(uint32_t now)
{
    if(!s_g.run_start_ms) return;
    s_g.played_ms=now-s_g.run_start_ms;
    s_g.level_time_ms=now-s_g.level_start_ms;
    if((int32_t)(now-s_g.combo_until)>=0) s_g.combo=0;
    s_g.multiplier=combo_mult();
}

void serpent_arcade_tick(uint32_t now)
{
    s_tick_now=now;
    rumble_tick(now);
    if(s_arc==ARC_OFF) return;
    if(s_arc==ARC_PLAY || s_arc==ARC_PAUSED) update_play_time(now);
    update_power_expiry(now);

    switch(s_arc) {
        case ARC_MENU:
            if(now-s_last_render>=FRAME_MS) { s_last_render=now; render_menu(now); }
            break;
        case ARC_COUNTDOWN:
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_countdown(now);}
            if(now-s_state_t0>=2400u){s_arc=ARC_INTRO;s_state_t0=now;}
            break;
        case ARC_INTRO:
            if((int32_t)(now-s_last_render)>=100){s_last_render=now;render_intro();}
            if(now-s_state_t0>=850u){
                s_arc=ARC_PLAY; s_state_t0=now; s_g.last_tick=now; s_g.acc=0; s_last_render=0;
                game_sound("ui_toggle_on",900);
            }
            break;
        case ARC_PLAY: {
            uint32_t dt=now-s_g.last_tick;
            if(dt>250u) dt=250u;
            s_g.last_tick=now;
            s_g.acc+=dt;
            s_g.played_ms=now-s_g.run_start_ms;
            s_g.level_time_ms=now-s_g.level_start_ms;
            particles_tick(dt);
            uint16_t step=effective_step();
            uint8_t guard=0;
            while(s_g.acc>=step && s_g.alive && guard++<4) {
                s_g.acc-=step;
                do_step(now);
                step=effective_step();
            }
            if(!s_g.alive) { s_arc=ARC_DYING; s_state_t0=now; }
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_play(now);}
            if((int32_t)(now-s_g.combo_until)>=0) s_g.combo=0;
            if(s_g.step_ms>0) s_g.multiplier=combo_mult();
            break;
        }
        case ARC_PAUSED:
            if((int32_t)(now-s_last_render)>=200){s_last_render=now;render_play(now);}
            break;
        case ARC_DYING:
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_dying(now);}
            if(now-s_fx_t0>=1200u) {
                if(s_g.lives>1) {
                    uint8_t lives=(uint8_t)(s_g.lives-1);
                    uint8_t practice=s_g.practice, endless=s_g.endless;
                    uint32_t score=s_g.score, foods=s_g.run_foods, bonus=s_g.bonuses;
                    uint32_t runstart=s_g.run_start_ms; uint32_t maxc=s_g.max_combo; uint32_t longest=s_g.longest_len;
                    uint8_t efood=s_g.endless_food;
                    start_level(s_g.level,now,1);
                    s_g.lives=lives; s_g.practice=practice; s_g.endless=endless; s_g.score=score;
                    s_g.run_foods=foods; s_g.bonuses=bonus; s_g.run_start_ms=runstart; s_g.max_combo=(uint8_t)maxc; s_g.longest_len=longest; s_g.endless_food=efood;
                    s_arc=ARC_RETRY; s_state_t0=now;
                } else game_over(now);
            }
            break;
        case ARC_RETRY:
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_intro();}
            if(now-s_state_t0>=900u){s_arc=ARC_INTRO;s_state_t0=now;}
            break;
        case ARC_CLEAR:
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_clear(now);}
            if(now-s_fx_t0>=800u) {
                if(s_g.level>=20) {
                    uint32_t score=s_g.score, runstart=s_g.run_start_ms, foods=s_g.run_foods, bonus=s_g.bonuses;
                    uint8_t lives=s_g.lives, practice=s_g.practice, maxc=s_g.max_combo;
                    uint32_t longest=s_g.longest_len;
                    start_level(20,now,1);
                    s_g.endless=1; s_g.lives=lives; s_g.practice=practice; s_g.score=score;
                    s_g.run_start_ms=runstart; s_g.run_foods=foods; s_g.bonuses=bonus;
                    s_g.max_combo=maxc; s_g.longest_len=longest; s_g.endless_food=0; s_g.food_in_level=0;
                    s_g.level_score=score;
                } else {
                    uint8_t nl=(uint8_t)(s_g.level+1), lives=s_g.lives, practice=s_g.practice;
                    uint32_t score=s_g.score, runstart=s_g.run_start_ms, foods=s_g.run_foods, bonus=s_g.bonuses;
                    uint8_t maxc=s_g.max_combo; uint32_t longest=s_g.longest_len;
                    start_level(nl,now,1);
                    s_g.level=nl; s_g.lives=lives; s_g.practice=practice; s_g.score=score;
                    s_g.run_start_ms=runstart; s_g.run_foods=foods; s_g.bonuses=bonus; s_g.max_combo=maxc; s_g.longest_len=longest; s_g.level_score=score;
                }
                s_arc=ARC_COUNTDOWN; s_state_t0=now; s_cd_step=99;
            }
            break;
        case ARC_OVER:
            if(now-s_last_render>=FRAME_MS){s_last_render=now;render_over(now);}
            break;
        default: break;
    }
}

void serpent_arcade_get_status(serpent_status_t *out)
{
    if(!out)return;
    const sp_level_t *L=sp_level(s_g.level);
    memset(out,0,sizeof(*out));
    out->state=(uint8_t)s_arc;
    out->score=s_g.score; out->hi=s_hiscore;
    out->level=s_g.level; out->level_count=20; out->level_name=L->name;
    out->food_in_level=s_g.food_in_level; out->food_target=s_g.endless?0:L->food_target;
    out->len=s_g.length; out->step_ms=effective_step(); out->wrap=L->wrap;
    uint8_t active_h=0; for(uint8_t i=0;i<HAZARD_MAX;i++) if(s_g.hz[i].active) active_h++;
    out->mines=(uint8_t)(s_g.mine_count+active_h);
    out->rumble_mode=s_rumble_mode; out->new_hi=s_new_hiscore;
    out->practice=s_g.practice; out->endless=s_g.endless; out->death_reason=s_last_dead;
    out->bonus_ms_left=0; out->power_ms_left=0;
    if(s_g.power_until) {
        int32_t left=(int32_t)(s_g.power_until-s_tick_now);
        if(left>0) out->power_ms_left=(uint16_t)(left>65535?65535:left);
    }
    out->best_level=s_best_level; out->lives=s_g.lives;
    out->fw=matrix_field_w(); out->fh=matrix_field_h(); out->winfx=s_g.winfx; out->pad_lost=s_pad_lost;
    out->combo=s_g.combo; out->multiplier=s_g.multiplier; out->max_combo=s_g.max_combo;
    out->power=(uint8_t)(s_g.active_power!=POWER_NONE?s_g.active_power:s_g.power_kind); out->shield=s_g.shield; out->slow=s_g.slow; out->ghost=s_g.ghost;
    out->foods_total=s_total_foods;
    out->longest_len=(uint32_t)(s_g.longest_len>s_all_longest?s_g.longest_len:s_all_longest);
    out->play_time_ms=s_g.played_ms; out->level_time_ms=s_g.level_time_ms;
    out->level_score=(s_g.score>=s_g.level_score)?(s_g.score-s_g.level_score):0; out->bonuses=s_g.bonuses;
    for(int i=0;i<120;i++) {
        out->frame_rgb[i][0]=s_mirror[i][0]; out->frame_rgb[i][1]=s_mirror[i][1]; out->frame_rgb[i][2]=s_mirror[i][2];
    }
    for(int i=0;i<4;i++){out->mirror_dirty[i]=s_mdirt[i];s_mdirt[i]=0;}
}

/* Compatibility stubs for the old TFT game registry. */
void serpent_mx_init(void) {}
void serpent_mx_update(const xbox360_pad_t *pad,uint32_t now_ms){(void)pad;(void)now_ms;}
void serpent_mx_draw(uint32_t now_ms){(void)now_ms;}

bool serpent_arcade_route(const xbox360_pad_t *pad,uint16_t dig,uint16_t tap,uint32_t now)
{
    bool present=pad&&pad->present&&xbox360_dongle_connected();
    rumble_tick(now);
    if((tap&B_GUIDE) && s_arc!=ARC_OFF){ exit_full(now); return false; }
    if(g.estop && s_arc!=ARC_OFF){ exit_full(now); return false; }
    if(dig&B_BACK){ if(!s_back_t0)s_back_t0=now; }
    else s_back_t0=0;
    bool back_hold=s_back_t0 && now-s_back_t0>=BACK_HOLD_MS;

    if(s_arc==ARC_OFF) {
        if(present && !g.estop && !os_game_active() && os_parked() && back_hold) {
            s_back_t0=0; enter_menu(now); return true;
        }
        return false;
    }

    if(!present) {
        s_pad_lost=1;
        if(s_arc==ARC_PLAY){s_arc=ARC_PAUSED;s_state_t0=now;led_matrix_set_dim(true);rumble_stop_all();game_sound("sys_disconnect",300);}
        return true;
    }
    if(s_pad_lost){s_pad_lost=0;input_flush_context_switch();game_sound("sys_connect",700);}
    led_matrix_set_dim(false);

    /* ONLY the face buttons are gameplay directions. D-pad/sticks/triggers are never read. */
    bool has_dir=false;
    direction_t fb=s_g.dir;
    if(dig&B_Y){fb=DIR_UP;has_dir=true;}
    else if(dig&B_A){fb=DIR_DOWN;has_dir=true;}
    else if(dig&B_X){fb=DIR_LEFT;has_dir=true;}
    else if(dig&B_B){fb=DIR_RIGHT;has_dir=true;}

    if(s_arc==ARC_MENU) {
        /* Menu level select uses the same four face buttons so D-pad remains unnecessary. */
        if((tap&B_Y) || (tap&B_X)) { if(s_sel_level<s_best_level)s_sel_level++; game_sound("ui_nav",800); }
        else if((tap&B_A) || (tap&B_B)) { if(s_sel_level>1)s_sel_level--; game_sound("ui_nav",600); }
        if(tap&B_LB){if(s_rumble_mode>0)s_rumble_mode--;save_stats();game_sound("ui_toggle_off",500);}
        if(tap&B_RB){if(s_rumble_mode<2)s_rumble_mode++;save_stats();game_sound("ui_toggle_on",900);}
        if(tap&B_START){
            uint8_t practice=s_sel_level>1?1:0;
            begin_run(s_sel_level,now,practice);
            s_total_games++;
            s_last_dead=DEAD_NONE; s_new_hiscore=0; s_pad_lost=0;
            s_arc=ARC_COUNTDOWN; s_state_t0=now; s_cd_step=99;
            game_sound("ui_tap",900); rb_tick();
        } else if(back_hold) {exit_full(now); return false;}
        serpent_arcade_tick(now); return true;
    }

    /* Ignore all non-face buttons during gameplay. */
    if(s_arc==ARC_COUNTDOWN || s_arc==ARC_INTRO || s_arc==ARC_PLAY) {
        if(has_dir) {
            static uint8_t armed=1;
            if(!armed) {
                if(!(dig&(B_A|B_B|B_X|B_Y))) armed=1;
            } else {
                /* input arming handled by face-button edge buffering below */
            }
        }
    }

    /* Face-button direction buffering. */
    if(has_dir && (s_arc==ARC_COUNTDOWN || s_arc==ARC_INTRO || s_arc==ARC_PLAY)) {
        direction_t ref=s_g.queue_n?s_g.queue[s_g.queue_n-1]:s_g.last_applied;
        bool reverse=(ref==DIR_UP&&fb==DIR_DOWN)||(ref==DIR_DOWN&&fb==DIR_UP)||
                     (ref==DIR_LEFT&&fb==DIR_RIGHT)||(ref==DIR_RIGHT&&fb==DIR_LEFT);
        if(fb!=ref && !reverse && s_g.queue_n<2) s_g.queue[s_g.queue_n++]=fb;
    }

    if(s_arc==ARC_PLAY && (tap&B_START)) {
        s_arc=ARC_PAUSED; s_state_t0=now; rumble_stop_all(); game_sound("ui_open_sheet",600);
    } else if(s_arc==ARC_PAUSED && (tap&B_START)) {
        s_arc=ARC_PLAY; s_g.last_tick=now; s_g.acc=0; game_sound("ui_close_sheet",700);
    }
    if(s_arc==ARC_PLAY && (tap&B_BACK)) {s_back_t0=0;enter_menu(now);return true;}
    if(s_arc==ARC_PAUSED && (tap&B_BACK)) {s_back_t0=0;enter_menu(now);return true;}
    if(s_arc==ARC_OVER) {
        if((tap&B_START) && now-s_over_t0>600u) {
            begin_run(s_sel_level,now,s_sel_level>1);
            s_total_games++; s_new_hiscore=0; s_arc=ARC_COUNTDOWN;s_state_t0=now;s_cd_step=99;game_sound("ui_tap",900);
        } else if(tap&B_BACK){s_new_hiscore=0;enter_menu(now);return true;}
    }
    if(back_hold && (s_arc==ARC_PLAY || s_arc==ARC_PAUSED || s_arc==ARC_OVER)){exit_full(now);return false;}
    serpent_arcade_tick(now);
    return true;
}

