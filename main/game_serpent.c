/*
 * game_serpent.c - NEON SERPENT: 10-level Nokia-style (blueprint v1.0 + 3 lives).
 *
 * States: OFF/MENU/COUNTDOWN/INTRO/PLAY/PAUSED/DYING/CLEAR/OVER (+ENDLESS).
 * Entry: BACK hold 1s while parked. Face buttons X/Y/B/A = directions.
 * START = begin/pause/resume/retry. BACK tap = menu, BACK hold = exit.
 * GUIDE never consumed (car E-STOP). RB/LB unused in play (no boost/dash).
 * MENU: Y/A = start-level select, LB/RB = rumble mode.
 */
#include "game_serpent.h"
#include "serpent_levels.h"
#include "led_matrix.h"
#include "car_global.h"
#include "car_os.h"
#include "notif.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "input_events.h"   /* context-switch flush (T24) */
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "serpent";

#define SERPENT_MAX_LEN 80
#define BACK_HOLD_MS 1000
#define FIRST_MOVE_DELAY 500

/* field size follows matrix orientation (12x10 landscape, 10x12 portrait) */
static uint8_t FW = 12, FH = 10;
#define OI(x, y) ((y) * FW + (x))
static uint16_t s_prows[12];      /* transposed wall masks (portrait) */
static uint8_t s_corr_lo = 4, s_corr_hi = 6;  /* mine-free start corridor rows */

static void field_refresh(void)
{
    FW = matrix_field_w();
    FH = matrix_field_h();
}

typedef struct { int8_t x, y; } cell_t;
typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } direction_t;
typedef enum {
    ARC_OFF, ARC_MENU, ARC_COUNTDOWN, ARC_INTRO, ARC_PLAY,
    ARC_PAUSED, ARC_DYING, ARC_RETRY, ARC_CLEAR, ARC_OVER
} arc_state_t;
typedef enum { DEAD_NONE, DEAD_WALL, DEAD_SELF, DEAD_MINE } dead_t;

typedef struct {
    cell_t body[SERPENT_MAX_LEN];
    uint8_t length;
    direction_t dir, last_applied;
    direction_t queue[2];
    uint8_t queue_n;
    cell_t food;
    cell_t mines[3];
    uint8_t mine_count;
    cell_t bonus;
    bool bonus_on;
    uint32_t bonus_t0, bonus_until;
    cell_t patrol;
    bool patrol_on;
    int8_t patrol_dx;
    uint32_t score;
    uint8_t level;            /* 1..10 */
    uint8_t lives;            /* 3 per run */
    uint8_t food_in_level;
    uint8_t endless_food;     /* foods eaten in ENDLESS (speed rule) */
    bool winfx;               /* WIN_BOARD rainbow wipe */
    uint16_t step_ms;
    uint32_t acc;             /* step accumulator */
    uint32_t last_tick;
    uint32_t next_step_at;    /* first-move delay anchor */
    bool alive;
    bool endless;
    bool practice;
    dead_t dead_why;
    uint8_t occ[120];         /* 0 free 1 snake 2 wall 3 mine 4 food 5 bonus */
} serpent_game_t;

static arc_state_t s_arc = ARC_OFF;
static serpent_game_t s_g;
static uint32_t s_state_t0 = 0, s_last_render = 0, s_cd_step = 0;
static uint32_t s_over_t0 = 0, s_fx_t0 = 0;
static bool s_new_hiscore = false;
static uint32_t s_hiscore = 0;
static uint8_t s_best_level = 1;
static uint8_t s_sel_level = 1;    /* menu start-level select */
static uint8_t s_rumble_mode = 1;  /* default SOFT (blueprint v2.0) */
static bool s_inited = false;
static uint32_t s_back_t0 = 0, s_b_t0 = 0;
static uint32_t s_rumble_until = 0;
static uint32_t s_dbg = 0;
static uint32_t s_last_step_ms = 0;   /* P5: step interval stats */
static uint32_t s_step_min = 0xFFFFFFFF, s_step_max = 0;
static uint32_t s_stats_t0 = 0;
static uint32_t s_tick_now = 0;
static uint8_t s_last_dead = 0;       /* death reason for TFT */
/* TFT mirror: logical-index copy + dirty bits (same task, no lock) */
static uint8_t s_mirror[120][3];
static uint32_t s_mdirt[4];
static void snk_mdirt_set(int idx)
{
    if (idx >= 0 && idx < 120) s_mdirt[idx >> 5] |= (1u << (idx & 31));
}
static void snk_px(int x, int y, rgb_t c)
{
    int fw = matrix_field_w(), fh = matrix_field_h();
    if (x < 0 || x >= fw || y < 0 || y >= fh) return;
    int idx = y * fw + x;
    s_mirror[idx][0] = c.r; s_mirror[idx][1] = c.g; s_mirror[idx][2] = c.b;
    snk_mdirt_set(idx);
    snk_px(x, y, c);
}
static void snk_clear(void)
{
    for (int i = 0; i < 120; i++) {
        s_mirror[i][0] = s_mirror[i][1] = s_mirror[i][2] = 0;
        snk_mdirt_set(i);
    }
    matrix_clear();
}

static const rgb_t C_BODY  = {0, 200, 60};
static const rgb_t C_HEAD  = {170, 255, 170};
static const rgb_t C_FOOD  = {255, 30, 30};
static const rgb_t C_MINE  = {150, 40, 200};
static const rgb_t C_BONUS = {255, 200, 40};
static const rgb_t C_BLOCK = {40, 50, 70};
static const rgb_t C_EDGE  = {0, 12, 48};

/* 3x5 digits */
static const uint8_t DIG[10][5] = {
    {0x7,0x5,0x5,0x5,0x7}, {0x2,0x6,0x2,0x2,0x7},
    {0x7,0x1,0x7,0x4,0x7}, {0x7,0x1,0x7,0x1,0x7},
    {0x5,0x5,0x7,0x1,0x1}, {0x7,0x4,0x7,0x1,0x7},
    {0x7,0x4,0x7,0x5,0x7}, {0x7,0x1,0x1,0x2,0x2},
    {0x7,0x5,0x7,0x5,0x7}, {0x7,0x5,0x7,0x1,0x7},
};

/* ---------------- rumble queue (v2.1 Sec 11, non-blocking) ---------------- */
typedef struct { uint8_t l, r; uint16_t ms; uint8_t sev; } rbq_t;
static rbq_t s_rbq[8];
static uint8_t s_rbq_n = 0;
static uint8_t s_rb_cur_sev = 0;
static uint32_t s_rb_cur_until = 0;

enum { SEV_TICK, SEV_EAT, SEV_BONUS, SEV_LEVELUP, SEV_CRASH, SEV_HISCORE };

static void rb_raw(uint8_t l, uint8_t r, uint32_t ms, uint32_t now)
{
    /* mode scale + <20 skip (same rule as car helper) */
    if (!s_rumble_mode) return;
    if (s_rumble_mode == 1) { l /= 2; r /= 2; }
    if (l < 20 && r < 20) return;
    xbox360_rumble(0, l, r);
    s_rumble_until = now + ms;
    s_rb_cur_until = now + ms;
}
static void rb_play(uint8_t l, uint8_t r, uint16_t ms, uint8_t sev)
{
    if (sev < s_rb_cur_sev && s_rbq_n >= 8) return;
    if (sev > s_rb_cur_sev) {   /* preempt lower */
        s_rbq_n = 0;
        s_rb_cur_sev = sev;
        xbox360_rumble(0, 0, 0);
    }
    if (s_rbq_n >= 8) return;
    s_rbq[s_rbq_n].l = l; s_rbq[s_rbq_n].r = r;
    s_rbq[s_rbq_n].ms = ms; s_rbq[s_rbq_n].sev = sev;
    s_rbq_n++;
}
/* patterns (FINAL strengths at FULL; SOFT scales inside rb_raw) */
static void rb_eat(void)                      { rb_play(0, 150, 60, SEV_EAT); }
static void rb_bonus(void) {
    rb_play(0, 200, 45, SEV_BONUS); rb_play(0, 0, 45, SEV_BONUS);
    rb_play(0, 200, 45, SEV_BONUS);
}
static void rb_levelup(void) {
    rb_play(60, 80, 100, SEV_LEVELUP); rb_play(110, 120, 100, SEV_LEVELUP);
    rb_play(160, 160, 100, SEV_LEVELUP); rb_play(210, 200, 120, SEV_LEVELUP);
}
static void rb_crash(void) {
    rb_play(255, 255, 220, SEV_CRASH); rb_play(190, 160, 120, SEV_CRASH);
    rb_play(120, 90, 120, SEV_CRASH); rb_play(60, 40, 120, SEV_CRASH);
}
static void rb_life(void) {   /* first two crash steps */
    rb_play(255, 255, 220, SEV_CRASH); rb_play(190, 160, 120, SEV_CRASH);
}
static void rb_hiscore(void) {
    for (int i = 0; i < 5; i++)
        rb_play((i & 1) ? 200 : 0, (i & 1) ? 0 : 200, 80, SEV_HISCORE);
}
static void rb_tick(void)                     { rb_play(0, 100, 30, SEV_TICK); }
static void rumble_tick(uint32_t now)
{
    if (s_rb_cur_until && (int32_t)(now - s_rb_cur_until) >= 0) {
        s_rb_cur_until = 0; s_rb_cur_sev = 0;
        if (!s_rbq_n) { xbox360_rumble(0, 0, 0); s_rumble_until = 0; }
    }
    if (!s_rb_cur_until && s_rbq_n) {
        rbq_t s = s_rbq[0];
        for (int i = 1; i < s_rbq_n; i++) s_rbq[i - 1] = s_rbq[i];
        s_rbq_n--;
        s_rb_cur_sev = s.sev;
        rb_raw(s.l, s.r, s.ms, now);
        if (!s_rumble_until) s_rb_cur_sev = 0;   /* skipped: free sev */
    }
}
static void rumble_stop_all(uint32_t now)
{
    (void)now;
    s_rbq_n = 0; s_rb_cur_sev = 0; s_rb_cur_until = 0; s_rumble_until = 0;
    xbox360_rumble(0, 0, 0);
}
static void arc_rumble(uint8_t l, uint8_t r, uint32_t ms, uint32_t now)
{
    (void)now;
    rb_play(l, r, (uint16_t)(ms > 0xFFFF ? 0xFFFF : ms), SEV_EAT);
}
static void arc_rumble_stop(uint32_t now)
{
    rumble_tick(now);
}

/* ---------------- grid helpers ---------------- */
static bool is_wall_cell(int x, int y, const sp_level_t *L)
{
    if (x < 0 || x >= FW || y < 0 || y >= FH) return true;
    if (matrix_get_orient() == MATRIX_ORIENT_PORTRAIT) {
        if (((s_prows[y] >> x) & 1)) return true;
    } else if ((L->block_rows[y] >> x) & 1) return true;
    if (!L->wrap && (x == 0 || x == FW - 1 || y == 0 || y == FH - 1)) return true;
    return false;
}
static void rebuild_occ(const sp_level_t *L)
{
    for (int i = 0; i < 120; i++) s_g.occ[i] = 0;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
            if (is_wall_cell(x, y, L)) s_g.occ[y * FW + x] = 2;
    for (int i = 0; i < s_g.length; i++)
        s_g.occ[s_g.body[i].y * FW + s_g.body[i].x] = 1;
    for (int i = 0; i < s_g.mine_count; i++)
        s_g.occ[s_g.mines[i].y * FW + s_g.mines[i].x] = 3;
    if (s_g.patrol_on) s_g.occ[s_g.patrol.y * FW + s_g.patrol.x] = 3;
    s_g.occ[s_g.food.y * FW + s_g.food.x] = 4;
    if (s_g.bonus_on)
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                s_g.occ[(s_g.bonus.y + dy) * FW + s_g.bonus.x + dx] = 5;
}
/* BFS reachable from head (wrap-aware). */
static bool bfs_reachable(int tx, int ty, const sp_level_t *L)
{
    static uint8_t seen[120];
    static uint8_t q[120];
    for (int i = 0; i < 120; i++) seen[i] = 0;
    int qh = 0, qt = 0;
    int si = s_g.body[0].y * FW + s_g.body[0].x;
    q[qt++] = (uint8_t)si; seen[si] = 1;
    const int8_t DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
    while (qh < qt) {
        int ci = q[qh++];
        int cx = ci % FW, cy = ci / FW;
        if (cx == tx && cy == ty) return true;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (L->wrap) {
                nx = (nx + FW) % FW; ny = (ny + FH) % FH;
            } else if (nx < 0 || nx >= FW || ny < 0 || ny >= FH) continue;
            int ni = ny * FW + nx;
            if (seen[ni]) continue;
            uint8_t c = s_g.occ[ni];
            if (c == 1 || c == 2 || c == 3) continue;  /* snake/wall/mine blocked */
            seen[ni] = 1;
            q[qt++] = (uint8_t)ni;
        }
    }
    return false;
}
static int manhattan(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2; if (dx < 0) dx = -dx;
    int dy = y1 - y2; if (dy < 0) dy = -dy;
    return dx + dy;
}
static void spawn_food(const sp_level_t *L)
{
    /* random EMPTY, head-dist >= 2, BFS reachable */
    for (int t = 0; t < 30; t++) {
        int x = (int)(esp_random() % FW);
        int y = (int)(esp_random() % FH);
        if (s_g.occ[y * FW + x]) continue;
        if (manhattan(x, y, s_g.body[0].x, s_g.body[0].y) < 2) continue;
        if (!bfs_reachable(x, y, L)) continue;
        s_g.food.x = (int8_t)x; s_g.food.y = (int8_t)y;
        s_g.occ[y * FW + x] = 4;
        return;
    }
}
static void place_mine(const sp_level_t *L)
{
    if (s_g.mine_count >= 3) return;
    for (int t = 0; t < 40; t++) {
        int x = (int)(esp_random() % FW);
        int y = (int)(esp_random() % FH);
        if (s_g.occ[y * FW + x]) continue;
        if (y >= s_corr_lo && y <= s_corr_hi) continue;  /* start corridor */
        if (manhattan(x, y, s_g.body[0].x, s_g.body[0].y) < 4) continue;
        s_g.mines[s_g.mine_count].x = (int8_t)x;
        s_g.mines[s_g.mine_count].y = (int8_t)y;
        s_g.mine_count++;
        rebuild_occ(L);
        /* food must stay reachable */
        if (bfs_reachable(s_g.food.x, s_g.food.y, L)) return;
        s_g.mine_count--;                            /* reject, disconnects */
        rebuild_occ(L);
    }
}
static void spawn_bonus(void)
{
    if (s_g.bonus_on) return;
    for (int t = 0; t < 40; t++) {
        int x = (int)(esp_random() % (FW - 1));
        int y = (int)(esp_random() % (FH - 1));
        bool free = true;
        for (int dy = 0; dy < 2 && free; dy++)
            for (int dx = 0; dx < 2 && free; dx++)
                if (s_g.occ[(y + dy) * FW + x + dx]) free = false;
        if (!free) continue;
        s_g.bonus.x = (int8_t)x; s_g.bonus.y = (int8_t)y;
        s_g.bonus_on = true;
        return;
    }
}

/* ---------------- level setup ---------------- */
static void start_level(uint8_t lv, uint32_t now, bool fresh_run)
{
    const sp_level_t *L = sp_level(lv);
    uint8_t slen = (uint8_t)(2 + lv);
    if (slen > 7) slen = 7;
    s_g.level = lv;
    s_g.length = slen;
    field_refresh();
    /* transpose walls once for portrait (12 rows x 10 bit) */
    if (matrix_get_orient() == MATRIX_ORIENT_PORTRAIT) {
        for (int py = 0; py < 12; py++) {
            uint16_t m = 0;
            for (int px = 0; px < 10; px++)
                if ((L->block_rows[9 - px] >> py) & 1) m |= (uint16_t)(1u << px);
            s_prows[py] = m;
        }
        /* portrait start: head (4,1) DOWN, column 4 clear everywhere */
        for (int i = 0; i < slen; i++) {
            s_g.body[i].x = 4;
            s_g.body[i].y = (int8_t)(1 + i);
        }
        s_g.dir = s_g.last_applied = DIR_DOWN;
        s_corr_lo = 0; s_corr_hi = 3;
    } else {
        for (int i = 0; i < slen; i++) {
            s_g.body[i].x = (int8_t)(6 - i);
            s_g.body[i].y = 5;
        }
        s_g.dir = s_g.last_applied = DIR_RIGHT;
        s_corr_lo = 4; s_corr_hi = 6;
    }
    s_g.queue_n = 0;
    s_g.mine_count = 0;
    s_g.food_in_level = 0;
    s_g.step_ms = L->step_ms;
    s_g.acc = 0;
    s_g.last_tick = now;
    s_g.next_step_at = now + FIRST_MOVE_DELAY;
    s_g.alive = true;
    s_g.dead_why = DEAD_NONE;
    s_g.bonus_on = false;
    s_g.patrol_on = false;
    s_g.winfx = false;
    s_g.food.x = -1; s_g.food.y = -1;
    for (int i = 0; i < 120; i++) s_g.occ[i] = 0;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
            if (is_wall_cell(x, y, L)) s_g.occ[y * FW + x] = 2;
    for (int i = 0; i < slen; i++)
        s_g.occ[s_g.body[i].y * FW + s_g.body[i].x] = 1;
    spawn_food(L);
    for (int i = 0; i < L->mines_start; i++) place_mine(L);
    if (L->patrol) {
        s_g.patrol_on = true;
        s_g.patrol_dx = 1;
        s_g.occ[L->patrol_row * FW + 0] = 3;
        s_g.patrol.x = 0; s_g.patrol.y = L->patrol_row;
    }
    if (fresh_run) {
        s_g.score = 0;
        s_g.lives = 3;
        s_g.endless = false;
        s_g.endless_food = 0;
    }
    s_last_step_ms = 0;   /* P5 stats reset per level */
    s_step_min = 0xFFFFFFFF; s_step_max = 0;
    ESP_LOGI(TAG, "level %d %s step=%d target=%d", lv, L->name,
             L->step_ms, L->food_target);
}

/* ---------------- direction queue ---------------- */
static bool queue_dir(direction_t d)
{
    direction_t ref = s_g.queue_n ? s_g.queue[s_g.queue_n - 1] : s_g.last_applied;
    if (d == ref) return false;                       /* same: ignore */
    if ((ref == DIR_UP && d == DIR_DOWN) ||
        (ref == DIR_DOWN && d == DIR_UP) ||
        (ref == DIR_LEFT && d == DIR_RIGHT) ||
        (ref == DIR_RIGHT && d == DIR_LEFT)) return false;  /* reversal */
    if (s_g.queue_n >= 2) return false;
    s_g.queue[s_g.queue_n++] = d;
    return true;
}

/* ---------------- step ---------------- */
static void do_step(uint32_t now)
{
    const sp_level_t *L = sp_level(s_g.level);
    if (s_g.queue_n) {
        s_g.dir = s_g.queue[0];
        s_g.queue[0] = s_g.queue[1];
        s_g.queue_n--;
    }
    s_g.last_applied = s_g.dir;
    cell_t nh = s_g.body[0];
    if (s_g.dir == DIR_UP) nh.y--;
    else if (s_g.dir == DIR_DOWN) nh.y++;
    else if (s_g.dir == DIR_LEFT) nh.x--;
    else nh.x++;
    if (L->wrap) {
        nh.x = (int8_t)((nh.x + FW) % FW);
        nh.y = (int8_t)((nh.y + FH) % FH);
    } else if (nh.x < 0 || nh.x >= FW || nh.y < 0 || nh.y >= FH) {
        s_g.alive = false; s_g.dead_why = DEAD_WALL; return;
    }
    int ni = nh.y * FW + nh.x;
    uint8_t c = s_g.occ[ni];
    if (c == 2) { s_g.alive = false; s_g.dead_why = DEAD_WALL; return; }
    if (c == 3) {
        s_g.alive = false;
        s_g.dead_why = (ni == s_g.patrol.y * FW + s_g.patrol.x && s_g.patrol_on)
                       ? DEAD_MINE : DEAD_MINE;
        return;
    }
    bool tail_leaves = true;
    bool eats = (c == 4), eats_bonus = (c == 5);
    if (c == 1) {
        int ti = s_g.body[s_g.length - 1].y * FW + s_g.body[s_g.length - 1].x;
        if (!(ni == ti)) { s_g.alive = false; s_g.dead_why = DEAD_SELF; return; }
        tail_leaves = false;   /* moving into tail: tail must stay this step */
    }
    /* grow */
    if (eats || eats_bonus) {
        if (s_g.length < SERPENT_MAX_LEN) {
            for (int i = s_g.length; i > 0; i--) s_g.body[i] = s_g.body[i - 1];
            s_g.length++;
        } else {
            for (int i = s_g.length - 1; i > 0; i--) s_g.body[i] = s_g.body[i - 1];
        }
        tail_leaves = false;
    } else {
        for (int i = s_g.length - 1; i > 0; i--) s_g.body[i] = s_g.body[i - 1];
    }
    s_g.body[0] = nh;
    rebuild_occ(L);
    if (eats) {
        uint8_t mult = s_g.level;
        s_g.score += 10u * mult;
        s_g.food_in_level++;
        car_sfx_score();
        rb_eat();
        if (s_g.endless) {
            /* B+: step = max(100, 120 - 20*floor(endless_food/5)) */
            s_g.endless_food++;
            uint16_t ns = (uint16_t)(120 - 20 * (s_g.endless_food / 5));
            if (ns < 100) ns = 100;
            s_g.step_ms = ns;
        }
        if (L->bonus_on && s_g.food_in_level % 4 == 0 && !s_g.bonus_on) {
            spawn_bonus();
            if (s_g.bonus_on) {
                s_g.bonus_t0 = now; s_g.bonus_until = now + 7000;
                rebuild_occ(L);
                car_sfx_blip(990);
            }
        }
        if (!s_g.endless && s_g.food_in_level >= L->food_target) return; /* clear */
        spawn_food(L);
        int maxm = L->mines_max;
        if (s_g.endless && maxm < 3) maxm = 3;
        if (s_g.food_in_level % 5 == 0 && s_g.mine_count < maxm &&
            s_g.mine_count < L->mines_max) place_mine(L);
        else if (s_g.food_in_level % 5 == 0 && s_g.mine_count < maxm) place_mine(L);
    }
    if (eats_bonus) {
        uint8_t mult = s_g.level;
        s_g.score += (now - s_g.bonus_t0 < 3000) ? 50u * mult : 20u * mult;
        s_g.bonus_on = false;
        rebuild_occ(L);
        car_sfx_score(); car_sfx_score();
        rb_bonus();
    }
    /* patrol every 2 steps */
    {
        static uint8_t s_pc = 0;
        if (s_g.patrol_on && ((++s_pc & 1) == 0)) {
            int nx = s_g.patrol.x + s_g.patrol_dx;
            int r = L->patrol_row;
            bool blocked = nx < 0 || nx >= FW || s_g.occ[r * FW + nx] == 2 ||
                           s_g.occ[r * FW + nx] == 1;
            if (blocked) s_g.patrol_dx = (int8_t)-s_g.patrol_dx;
            else { s_g.patrol.x = (int8_t)nx; rebuild_occ(L); }
        }
    }
    /* board full -> WIN_BOARD */
    if (s_g.alive) {
        bool any_free = false;
        for (int i = 0; i < 120; i++) {
            if (!s_g.occ[i]) { any_free = true; break; }
        }
        if (!any_free) s_g.winfx = true;
    }
    (void)tail_leaves;
}

/* ---------------- NVS ---------------- */
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("arcade", NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0; uint8_t b = 0;
        if (nvs_get_u32(h, "serp_hi2", &v) == ESP_OK) s_hiscore = v;
        if (nvs_get_u8(h, "serp_lvl", &b) == ESP_OK && b >= 1 && b <= 10) s_best_level = b;
        if (nvs_get_u8(h, "rumble", &b) == ESP_OK && b <= 2) s_rumble_mode = b;
        nvs_close(h);
    }
    s_sel_level = 1;
}
static void nvs_save_hi(void)
{
    nvs_handle_t h;
    if (nvs_open("arcade", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "serp_hi2", s_hiscore);
        nvs_set_u8(h, "serp_lvl", s_best_level);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- render ---------------- */
static void draw_digit(int x, int y, int d, rgb_t c)
{
    if (d < 0 || d > 9) return;
    for (int r = 0; r < 5; r++)
        for (int cc = 0; cc < 3; cc++)
            if (DIG[d][r] & (1u << (2 - cc)))
                snk_px(x + cc, y + r, c);
}
static void draw_digit2x(int x, int y, int d, rgb_t c)
{
    if (d < 0 || d > 9) return;
    for (int r = 0; r < 5; r++)
        for (int cc = 0; cc < 3; cc++)
            if (DIG[d][r] & (1u << (2 - cc)))
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        snk_px(x + cc * 2 + dx, y + r * 2 + dy, c);
}
static void draw_level_num(uint8_t lv, rgb_t c)
{
    int cx = (FW - 7) / 2;   /* 2-digit centred (7 col) */
    if (lv >= 10) { draw_digit(cx, 2, lv / 10, c); draw_digit(cx + 5, 2, lv % 10, c); }
    else draw_digit((FW - 3) / 2, 2, lv, c);
}
static void render_menu(uint32_t now)
{
    snk_clear();
    /* green snake zigzag + red food icon */
    bool on = ((now / 400) & 1) == 0;
    rgb_t snake = on ? C_BODY : (rgb_t){0, 60, 30};
    snk_px(1, 1, snake); snk_px(2, 1, snake);
    snk_px(3, 1, snake); snk_px(3, 2, snake);
    snk_px(4, 2, snake); snk_px(5, 2, snake);
    snk_px(5, 2, C_HEAD);
    snk_px(9, 1, C_FOOD);
    /* selected start level digits */
    draw_level_num(s_sel_level, (rgb_t){255, 200, 40});
    /* rumble mode pips bottom-left (0..2) */
    for (int i = 0; i < (int)s_rumble_mode; i++)
        snk_px(i, FH - 1, (rgb_t){0, 150, 200});
    matrix_show();
}
static void render_countdown(uint32_t now)
{
    uint32_t el = now - s_state_t0;
    uint32_t step = el / 700;   /* 0,1,2 then GO */
    if (step != s_cd_step) {
        s_cd_step = step;
        car_sfx_blip(step < 2 ? 440 : 880);
        rb_tick();
    }
    snk_clear();
    if (step < 3) draw_digit2x((FW - 6) / 2, 0, (int)(3 - step), (rgb_t){0, 200, 255});
    else {
        for (int x = 0; x < FW; x++) {   /* GO sweep */
            snk_px(x, 0, (rgb_t){0, 150, 60});
            snk_px(x, FH - 1, (rgb_t){0, 150, 60});
        }
    }
    matrix_show();
}
static void render_intro(void)
{
    snk_clear();
    draw_level_num(s_g.level, (rgb_t){255, 200, 0});
    /* lives dots hint (row 8, remaining bright) */
    for (int i = 0; i < 3; i++)
        snk_px(3 + i * 2, 8, i < s_g.lives ? C_FOOD : (rgb_t){60, 10, 10});
    matrix_show();
}
static void render_play(uint32_t now)
{
    const sp_level_t *L = sp_level(s_g.level);
    snk_clear();
    /* WALL edge tint */
    if (!L->wrap) {
        for (int x = 0; x < FW; x++) {
            snk_px(x, 0, C_EDGE);
            snk_px(x, FH - 1, C_EDGE);
        }
        for (int y = 0; y < FH; y++) {
            snk_px(0, y, C_EDGE);
            snk_px(FW - 1, y, C_EDGE);
        }
    }
    /* blocks (portrait uses transposed masks) */
    bool port = matrix_get_orient() == MATRIX_ORIENT_PORTRAIT;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++)
            if (port ? ((s_prows[y] >> x) & 1)
                     : ((L->block_rows[y] >> x) & 1))
                snk_px(x, y, C_BLOCK);
    /* mines static + patrol pulse */
    for (int i = 0; i < s_g.mine_count; i++)
        snk_px(s_g.mines[i].x, s_g.mines[i].y, C_MINE);
    if (s_g.patrol_on) {
        bool p = ((now / 160) & 1) == 0;
        rgb_t c = p ? C_MINE : (rgb_t){80, 20, 100};
        snk_px(s_g.patrol.x, s_g.patrol.y, c);
    }
    /* steady red food */
    snk_px(s_g.food.x, s_g.food.y, C_FOOD);
    /* bonus: 4 Hz blink last 2 s */
    if (s_g.bonus_on) {
        int32_t left = (int32_t)(s_g.bonus_until - now);
        bool show = left < 2000 ? ((now / 125) & 1) == 0
                                : ((now / 300) & 1) == 0;
        if (show)
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++)
                    snk_px(s_g.bonus.x + dx, s_g.bonus.y + dy, C_BONUS);
    }
    /* snake gradient + lime head */
    for (int i = (int)s_g.length - 1; i >= 0; i--) {
        rgb_t c;
        if (i == 0) c = C_HEAD;
        else {
            uint8_t v = (uint8_t)(200 - (i * 160) / SERPENT_MAX_LEN);
            if (v < 40) v = 40;
            c.r = 0; c.g = v; c.b = (uint8_t)(v / 4);
        }
        snk_px(s_g.body[i].x, s_g.body[i].y, c);
    }
    matrix_show();
}
static void render_dying(uint32_t now)
{
    uint32_t el = now - s_fx_t0;
    snk_clear();
    if (car_get_mx_fx() == 0) {   /* MINIMAL: red flash only */
        if (((el / 200) & 1) == 0)
            for (int y = 0; y < FH; y++)
                for (int x = 0; x < FW; x++)
                    snk_px(x, y, C_FOOD);
        matrix_show();
        return;
    }
    if (el < 360) {
        /* collision cell blink x3 */
        if (((el / 120) & 1) == 0)
            for (int y = 0; y < FH; y++)
                for (int x = 0; x < FW; x++)
                    if (s_g.occ[y * FW + x] == 1)
                        snk_px(x, y, C_FOOD);
    } else {
        /* red dissolve head->tail */
        uint32_t k = (el - 360) * s_g.length / 840;
        for (int i = 0; i < (int)s_g.length; i++) {
            rgb_t c = (uint32_t)i < k ? C_FOOD
                : (rgb_t){0, (uint8_t)(200 - i * 160 / SERPENT_MAX_LEN), 0};
            snk_px(s_g.body[i].x, s_g.body[i].y, c);
        }
    }
    matrix_show();
}
static void render_clear(uint32_t now)
{
    if (car_get_mx_fx() == 0) { snk_clear(); matrix_show(); return; }
    if (s_g.winfx) {   /* WIN_BOARD rainbow */
        int ph = (int)((now / 120) % 6);
        static const rgb_t rb[6] = {
            {220,0,0},{255,150,0},{255,255,0},{0,180,35},{0,110,180},{130,0,180}
        };
        snk_clear();
        for (int y = 0; y < FH; y++)
            for (int x = 0; x < FW; x++)
                snk_px(x, y, rb[(x + y + ph) % 6]);
        matrix_show();
        return;
    }
    uint32_t row = (now - s_fx_t0) / 40;   /* green wipe top->bottom */
    snk_clear();
    for (uint32_t y = 0; y < row && y < 10; y++)
        for (int x = 0; x < FW; x++)
            snk_px(x, y, (rgb_t){0, 180, 60});
    matrix_show();
}
static void render_over(uint32_t now)
{
    uint32_t el = now - s_over_t0;
    snk_clear();
    if (el < 100) {
        for (int y = 0; y < FH; y++)
            for (int x = 0; x < FW; x++)
                snk_px(x, y, C_FOOD);
        matrix_show();
        return;
    }
    draw_level_num(s_g.level, (rgb_t){255, 60, 60});
    /* score bar bottom (gold, capped 24) */
    uint32_t sc = s_g.score > 240 ? 24 : (s_g.score + 9) / 10;
    for (uint32_t i = 0; i < sc; i++)
        snk_px((int)(i % FW), (FH - 1) - (int)(i / FW), (rgb_t){255, 200, 40});
    if (s_new_hiscore) {
        int ph = (int)((now / 120) % 6);
        static const rgb_t rb[6] = {
            {220,0,0},{255,150,0},{255,255,0},{0,180,35},{0,110,180},{130,0,180}
        };
        for (int x = 0; x < FW; x++) {
            snk_px(x, 0, rb[(x + ph) % 6]);
            snk_px(x, FH - 1, rb[(x + ph + 3) % 6]);
        }
    }
    matrix_show();
}

/* ---------------- state changes ---------------- */
static void enter_menu(uint32_t now)
{
    s_arc = ARC_MENU;
    s_state_t0 = now;
    if (s_sel_level < 1 || s_sel_level > s_best_level) s_sel_level = 1;
    led_matrix_acquire(OWNER_ARCADE);
    led_matrix_set_demo(false);
    led_matrix_set_dim(false);
    matrix_set_dim_pct(100);
    input_flush_context_switch();   /* hub-held buttons must not leak in */
    car_sfx_click();
    ESP_LOGI(TAG, "arcade MENU (START=start Y/A=level LB/RB=rumble)");
    notif_push(NOTIF_RULES, "ARCADE: NEON SERPENT", now);
}
static void exit_full(uint32_t now)
{
    (void)now;
    s_arc = ARC_OFF;
    s_back_t0 = s_b_t0 = 0;
    rumble_stop_all(now);
    if (!s_g.practice && s_g.score > s_hiscore && s_g.score > 0) {
        s_hiscore = s_g.score;
        nvs_save_hi();
    }
    led_matrix_safe_off();
    led_matrix_release(OWNER_ARCADE);
    led_matrix_set_demo(true);
    matrix_set_dim_pct(100);
    input_flush_context_switch();   /* game-held buttons must not leak out */
    /* hub-launched game: back to hub (HOME path stays where it was) */
    {
        os_ctx_t *oc = car_os_ctx();
        if (oc && oc->state == OS_GAME_3)
            os_request_screen(oc, OS_GAMES_HUB, now);
    }
    ESP_LOGI(TAG, "arcade OFF");
}
static void game_over(uint32_t now, bool won_endless)
{
    (void)won_endless;
    s_arc = ARC_OVER;
    s_state_t0 = now;
    s_over_t0 = now;
    s_new_hiscore = !s_g.practice && s_g.score > s_hiscore && s_g.score > 0;
    if (s_new_hiscore) {
        s_hiscore = s_g.score;
        nvs_save_hi();
        rb_hiscore();
    }
    if (s_g.level > s_best_level) { s_best_level = s_g.level; nvs_save_hi(); }
    car_sfx_bad();
    rb_crash();
    const char *why = s_g.dead_why == DEAD_WALL ? "WALL" :
                      s_g.dead_why == DEAD_SELF ? "SELF" : "MINE";
    ESP_LOGI(TAG, "game over lvl=%d score=%lu hi=%lu cause=%s%s",
             s_g.level, (unsigned long)s_g.score, (unsigned long)s_hiscore,
             why, s_new_hiscore ? " NEW HI!" : "");
    notif_push(NOTIF_RULES, s_new_hiscore ? "SERPENT NEW HISCORE" : "SERPENT GAME OVER", now);
}
static void level_clear(uint32_t now)
{
    const sp_level_t *L = sp_level(s_g.level);
    uint32_t bonus = 50u * s_g.level;
    if (s_g.winfx) bonus += 500u * s_g.level;   /* WIN_BOARD */
    s_g.score += bonus;
    car_sfx_score();
    rb_levelup();
    ESP_LOGI(TAG, "level %d clear +%lu score=%lu", s_g.level,
             (unsigned long)bonus, (unsigned long)s_g.score);
    if (s_g.level >= 10) {
        if (!s_g.endless) {
            s_g.endless = true;
            s_g.endless_food = 0;
            ESP_LOGI(TAG, "ENDLESS mode");
            notif_push(NOTIF_RULES, "SERPENT ENDLESS", now);
        }
        s_arc = ARC_CLEAR;   /* short fx then replay L10 faster */
    } else {
        if (s_g.level + 1 > s_best_level) {
            s_best_level = (uint8_t)(s_g.level + 1);
            nvs_save_hi();
        }
        s_arc = ARC_CLEAR;
    }
    s_fx_t0 = now;
    s_state_t0 = now;
    (void)L;
}

/* ---------------- public ---------------- */
void serpent_arcade_init(void)
{
    if (s_inited) return;
    s_inited = true;
    nvs_load();
    ESP_LOGI(TAG, "serpent v10 init hi=%lu best=%d rumble=%d",
             (unsigned long)s_hiscore, s_best_level, s_rumble_mode);
}
bool serpent_arcade_active(void) { return s_arc != ARC_OFF; }
uint32_t serpent_hiscore(void) { return s_hiscore; }
bool serpent_arcade_open(void)
{
    if (s_arc != ARC_OFF) return true;   /* already in */
    bool m = led_matrix_ready();
    bool padok = false;
    const xbox360_pad_t *p = xbox360_pad(0);
    if (p && p->present && xbox360_dongle_connected()) padok = true;
    if (!m || g.estop || os_game_active() || !os_parked() || !padok) {
        ESP_LOGW(TAG, "open blocked: matrix=%d estop=%d twgame=%d parked=%d pad=%d",
                 m, g.estop, os_game_active(), os_parked(), padok);
        return false;
    }
    enter_menu((uint32_t)(esp_timer_get_time() / 1000));
    return true;
}
uint8_t serpent_rumble_mode(void) { return s_rumble_mode; }
void serpent_set_rumble_mode(uint8_t m)
{
    if (m > 2) return;
    s_rumble_mode = m;
    nvs_handle_t h;
    if (nvs_open("arcade", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rumble", m);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint8_t serpent_arcade_state(void) { return (uint8_t)s_arc; }
bool serpent_suppress_oled(void)
{
    return s_arc == ARC_COUNTDOWN || s_arc == ARC_INTRO ||
           s_arc == ARC_PLAY || s_arc == ARC_DYING || s_arc == ARC_RETRY ||
           s_arc == ARC_CLEAR;
}

/* Per-loop game progression (B+): steps/timers/renders. Caller-independent —
   input handling stays in serpent_arcade_route. */
void serpent_arcade_tick(uint32_t now)
{
    s_tick_now = now;
    matrix_set_dim_pct(s_arc == ARC_PAUSED ? 25 : 100);   /* pause dim */
    switch (s_arc) {
    case ARC_MENU:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_menu(now);
        }
        break;
    case ARC_COUNTDOWN:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_countdown(now);
        }
        if (now - s_state_t0 >= 2100 + 300) {
            s_arc = ARC_INTRO;
            s_state_t0 = now;
        }
        break;
    case ARC_INTRO:
        if ((int32_t)(now - s_last_render) >= 100) {
            s_last_render = now;
            render_intro();
        }
        if (now - s_state_t0 >= 900) {
            s_arc = ARC_PLAY;
            s_state_t0 = now;
            s_g.last_tick = now;
            s_g.acc = 0;
            s_g.next_step_at = now + FIRST_MOVE_DELAY;
            ESP_LOGI(TAG, "GO!");
        }
        break;
    case ARC_PLAY: {
        uint32_t dt = now - s_g.last_tick;
        s_g.last_tick = now;
        if (dt > 100) dt = 100;
        const sp_level_t *L = sp_level(s_g.level);
        if (now >= s_g.next_step_at) {
            s_g.acc += dt;
            if (s_g.acc >= s_g.step_ms) {
                s_g.acc -= s_g.step_ms;
                if (s_g.acc > s_g.step_ms) s_g.acc = s_g.step_ms;
                if (s_last_step_ms) {   /* P5: step interval stats */
                    uint32_t iv = now - s_last_step_ms;
                    if (iv < s_step_min) s_step_min = iv;
                    if (iv > s_step_max) s_step_max = iv;
                }
                s_last_step_ms = now;
                uint8_t fil_before = s_g.food_in_level;
                do_step(now);
                if (!s_g.alive) {
                    s_last_dead = s_g.dead_why == DEAD_WALL ? 1 :
                                  s_g.dead_why == DEAD_SELF ? 2 : 3;
                    s_arc = ARC_DYING;
                    s_fx_t0 = now;
                    s_state_t0 = now;
                    car_sfx_bad();
                    /* RB_LIFE vs full crash */
                    if (s_g.lives > 1) rb_life();
                    else rb_crash();
                    break;
                }
                if (s_g.winfx) { level_clear(now); break; }   /* WIN_BOARD */
                if (!s_g.endless && s_g.food_in_level >= L->food_target &&
                    s_g.food_in_level != fil_before) {
                    level_clear(now);
                    break;
                }
            }
        }
        if (s_g.bonus_on && (int32_t)(now - s_g.bonus_until) >= 0) {
            s_g.bonus_on = false;
            rebuild_occ(L);
        }
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_play(now);
        }
        /* P5: PLAY stats every 60 s */
        if (!s_stats_t0) s_stats_t0 = now;
        if (now - s_stats_t0 >= 60000) {
            s_stats_t0 = now;
            ESP_LOGI(TAG, "play stats L%d step=%dms min/max=%lu/%lu score=%lu",
                     s_g.level, s_g.step_ms,
                     (unsigned long)s_step_min, (unsigned long)s_step_max,
                     (unsigned long)s_g.score);
            s_step_min = 0xFFFFFFFF; s_step_max = 0;
        }
        break;
    }
    case ARC_PAUSED:
        if ((int32_t)(now - s_last_render) >= 200) {
            s_last_render = now;
            render_play(now);
        }
        break;
    case ARC_DYING:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_dying(now);
        }
        if (now - s_fx_t0 >= 1300) {
            if (s_g.lives > 1) {
                s_g.lives--;
                ESP_LOGI(TAG, "life lost, %d left — replay L%d",
                         s_g.lives, s_g.level);
                bool en = s_g.endless;
                uint16_t sm = s_g.step_ms;
                start_level(s_g.level, now, false);  /* score/practice kept */
                if (en) { s_g.endless = true; s_g.step_ms = sm; }
                s_arc = ARC_RETRY;   /* digits + lives dots, then INTRO */
                s_state_t0 = now;
            } else {
                game_over(now, false);
            }
        }
        break;
    case ARC_RETRY:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_intro();   /* level digits + lives dots */
        }
        if (now - s_state_t0 >= 1000) {
            s_arc = ARC_INTRO;
            s_state_t0 = now;
        }
        break;
    case ARC_CLEAR:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_clear(now);
        }
        if (now - s_fx_t0 >= 500) {
            uint8_t nl = s_g.level >= 10 ? 10 : (uint8_t)(s_g.level + 1);
            uint32_t sc = s_g.score;
            bool pr = s_g.practice;
            uint8_t lv = s_g.lives;
            start_level(nl, now, false);
            s_g.score = sc; s_g.practice = pr; s_g.lives = lv;
            s_arc = ARC_COUNTDOWN;
            s_state_t0 = now;
            s_cd_step = 99;
        }
        break;
    case ARC_OVER:
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_over(now);
        }
        break;
    default:
        break;
    }
}

void serpent_arcade_get_status(serpent_status_t *out)
{
    if (!out) return;
    const sp_level_t *L = sp_level(s_g.level);
    out->state = (uint8_t)s_arc;
    out->score = s_g.score;
    out->hi = s_hiscore;
    out->level = s_g.level;
    out->level_count = 10;
    out->level_name = L->name;
    out->food_in_level = s_g.food_in_level;
    out->food_target = L->food_target;
    out->len = s_g.length;
    out->step_ms = s_g.step_ms;
    out->wrap = L->wrap;
    out->mines = (uint8_t)(s_g.mine_count + (s_g.patrol_on ? 1 : 0));
    out->rumble_mode = s_rumble_mode;
    out->new_hi = s_new_hiscore ? 1 : 0;
    out->practice = s_g.practice ? 1 : 0;
    out->endless = s_g.endless ? 1 : 0;
    out->death_reason = s_last_dead;
    out->bonus_ms_left = 0;
    if (s_g.bonus_on) {
        int32_t left = (int32_t)(s_g.bonus_until - s_tick_now);
        if (left > 0) out->bonus_ms_left = (uint16_t)(left > 0xFFFF ? 0xFFFF : left);
    }
    out->best_level = s_best_level;
    out->lives = s_g.lives;
    out->fw = matrix_field_w();
    out->fh = matrix_field_h();
    out->winfx = s_g.winfx ? 1 : 0;
    for (int i = 0; i < 120; i++) {
        out->frame_rgb[i][0] = s_mirror[i][0];
        out->frame_rgb[i][1] = s_mirror[i][1];
        out->frame_rgb[i][2] = s_mirror[i][2];
    }
    for (int i = 0; i < 4; i++) {
        out->mirror_dirty[i] = s_mdirt[i];
        s_mdirt[i] = 0;   /* reader clears */
    }
}

/* Games-Hub registry stubs (real game runs on matrix via route/tick) */
void serpent_mx_init(void) {}
void serpent_mx_update(const xbox360_pad_t *pad, uint32_t now_ms)
{
    (void)pad; (void)now_ms;
}
void serpent_mx_draw(uint32_t now_ms) { (void)now_ms; }

bool serpent_arcade_route(const xbox360_pad_t *pad, uint16_t dig,
                          uint16_t tap, uint32_t now)
{
    bool present = pad && pad->present && xbox360_dongle_connected();
    arc_rumble_stop(now);

    if ((tap & B_GUIDE) && s_arc != ARC_OFF) {
        exit_full(now);
        return false;
    }
    /* E-STOP latched elsewhere (Alexa/IMU): leave arcade, motors stay 0 */
    if (g.estop && s_arc != ARC_OFF) {
        exit_full(now);
        return false;
    }
    if (dig & B_BACK) { if (!s_back_t0) s_back_t0 = now; }
    else s_back_t0 = 0;
    if (dig & B_B) { if (!s_b_t0) s_b_t0 = now; }
    else s_b_t0 = 0;

    if (s_arc == ARC_OFF) {
        if (present && !g.estop && !os_game_active() && os_parked() &&
            s_back_t0 && now - s_back_t0 >= BACK_HOLD_MS) {
            s_back_t0 = 0;
            enter_menu(now);
            return true;
        }
        if (s_back_t0 && now - s_back_t0 >= 1200) {
            if ((int32_t)(now - s_dbg) >= 0) {
                s_dbg = now + 2000;
                ESP_LOGW(TAG, "arcade entry blocked: present=%d estop=%d twgame=%d parked=%d",
                         present, g.estop, os_game_active(), os_parked());
            }
        }
        return false;
    }

    if (!present) {
        if (s_arc == ARC_PLAY) {
            s_arc = ARC_PAUSED;
            s_state_t0 = now;
            led_matrix_set_dim(true);
            rumble_stop_all(now);
            ESP_LOGI(TAG, "pad lost -> paused+dim");
        }
        if ((int32_t)(now - s_last_render) >= 33) {
            s_last_render = now;
            render_play(now);
        }
        return true;
    }
    led_matrix_set_dim(false);

    /* face-button directions (all states that steer) */
    direction_t fb = s_g.dir;
    bool has_dir = false;
    if (dig & B_Y) { fb = DIR_UP; has_dir = true; }
    else if (dig & B_A) { fb = DIR_DOWN; has_dir = true; }
    else if (dig & B_X) { fb = DIR_LEFT; has_dir = true; }
    else if (dig & B_B) { fb = DIR_RIGHT; has_dir = true; }
    /* D-pad: edge (tap) queued */
    direction_t dp = s_g.dir;
    bool has_dp = false;
    if (tap & B_DUP) { dp = DIR_UP; has_dp = true; }
    else if (tap & B_DDOWN) { dp = DIR_DOWN; has_dp = true; }
    else if (tap & B_DLEFT) { dp = DIR_LEFT; has_dp = true; }
    else if (tap & B_DRIGHT) { dp = DIR_RIGHT; has_dp = true; }
    /* left stick: dominant axis, 8500 threshold, 1500 hysteresis, edge */
    direction_t st = s_g.dir;
    bool has_st = false;
    {
        static bool armed = true;
        int ax = pad->lx >= 0 ? pad->lx : -pad->lx;
        int ay = pad->ly >= 0 ? pad->ly : -pad->ly;
        if (ax < 7000 && ay < 7000) armed = true;
        else if (armed) {
            if (ax > ay && ax > 8500) {
                st = pad->lx > 0 ? DIR_RIGHT : DIR_LEFT; has_st = true;
            } else if (ay >= ax && ay > 8500) {
                st = pad->ly > 0 ? DIR_UP : DIR_DOWN; has_st = true;
            }
            if (has_st) armed = false;
        }
    }
    /* arming rule: ignore dirs until everything released once */
    {
        static bool g_armed = false;
        if (!has_dir && !has_dp && !has_st &&
            pad->lx > -7000 && pad->lx < 7000 &&
            pad->ly > -7000 && pad->ly < 7000) g_armed = true;
        if (!g_armed) { has_dir = has_dp = has_st = false; }
    }
    bool back_hold = s_back_t0 && now - s_back_t0 >= BACK_HOLD_MS;

    switch (s_arc) {
    case ARC_MENU:
        if (tap & B_LB) {
            if (s_rumble_mode > 0) { s_rumble_mode--; nvs_save_hi(); car_sfx_click(); }
        } else if (tap & B_RB) {
            if (s_rumble_mode < 2) { s_rumble_mode++; nvs_save_hi(); car_sfx_click(); }
        } else if ((has_dp && dp == DIR_UP) || (has_st && st == DIR_UP)) {
            if (s_sel_level < s_best_level) { s_sel_level++; car_sfx_click(); }
        } else if ((has_dp && dp == DIR_DOWN) || (has_st && st == DIR_DOWN)) {
            if (s_sel_level > 1) { s_sel_level--; car_sfx_click(); }
        } else if (tap & B_START) {
            s_g.practice = s_sel_level > 1;
            start_level(s_sel_level, now, true);
            s_arc = ARC_COUNTDOWN;
            s_state_t0 = now;
            s_cd_step = 99;
            car_sfx_click();
            rb_tick();
            ESP_LOGI(TAG, "countdown lvl=%d%s", s_sel_level,
                     s_g.practice ? " PRACTICE" : "");
        } else if (back_hold) {
            exit_full(now);
            return false;
        }
        break;

    case ARC_COUNTDOWN:
        if (has_st) queue_dir(st);
        if (has_dp) queue_dir(dp);
        if (has_dir) queue_dir(fb);   /* pre-steer */
        break;

    case ARC_INTRO:
        if (has_st) queue_dir(st);
        if (has_dp) queue_dir(dp);
        if (has_dir) queue_dir(fb);
        break;

    case ARC_PLAY: {
        if (has_st) queue_dir(st);
        if (has_dp) queue_dir(dp);
        if (has_dir) queue_dir(fb);
        if (tap & B_START) {
            s_arc = ARC_PAUSED;
            s_state_t0 = now;
            rumble_stop_all(now);
            ESP_LOGI(TAG, "paused");
        } else if (back_hold) {
            s_back_t0 = 0;
            enter_menu(now);
        }
        break;
    }

    case ARC_PAUSED:
        if (tap & B_START) {
            s_arc = ARC_PLAY;
            s_g.last_tick = now;   /* timers freeze: no dt jump */
            ESP_LOGI(TAG, "resumed");
        } else if (tap & B_BACK) {
            s_back_t0 = 0;
            enter_menu(now);
        }
        break;

    case ARC_DYING:
    case ARC_CLEAR:
        break;

    case ARC_OVER:
        if ((tap & B_START) && now - s_over_t0 > 600) {
            start_level(s_sel_level, now, true);
            s_g.practice = s_sel_level > 1;
            s_arc = ARC_COUNTDOWN;
            s_state_t0 = now;
            s_cd_step = 99;
            s_new_hiscore = false;
        } else if (tap & B_BACK) {
            s_back_t0 = 0;
            s_new_hiscore = false;
            enter_menu(now);
        }
        break;

    default:
        break;
    }
    serpent_arcade_tick(now);
    return true;
}
