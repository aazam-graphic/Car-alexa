/*
 * led_matrix.c - 120-LED RGB ARCADE MATRIX driver + diagnostics.
 * Blueprint sections 3, 4, 12, 13, 14 Phase-A/B.
 *
 * - Static 120x3 framebuffer (360 bytes, DRAM). No malloc in loop.
 * - Serpentine XY mapper (even rows L->R, odd rows R->L).
 * - Global brightness cap 72/255 enforced on every push.
 * - RMT non-DMA (rear strip owns DMA). Roof strip disabled (takeover).
 * - matrix_show() pacing caller-side (~30 FPS via led_matrix_tick).
 */
#include "led_matrix.h"
#include "falak_show.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "matrix";

static led_strip_handle_t s_strip = NULL;
static bool s_ready = false;
static bool s_demo = true;          /* test ke liye ON */
static bool s_dim = false;          /* disconnect dim mode */
static uint8_t s_dim_pct = 100;     /* extra dim % (pause 25) */
static matrix_owner_t s_owner = OWNER_FALAK;
static uint8_t s_bright = 50;       /* normal game brightness 35..72 */
static uint8_t s_orient = MATRIX_ORIENT_LANDSCAPE;
static uint8_t s_fw = LED_MATRIX_W, s_fh = LED_MATRIX_H;
static uint8_t s_idle = MATRIX_IDLE_FALAK;
static uint8_t s_fb[LED_MATRIX_COUNT][3];  /* [i][0]=R,[1]=G,[2]=B */
static uint32_t s_last_show = 0;
static uint8_t s_boot_step = 0;
static uint32_t s_boot_t0 = 0;

bool led_matrix_is_active(void)
{
#if LED_MATRIX_TAKEOVER_ROOF
    return true;
#else
    return s_ready;
#endif
}

bool led_matrix_ready(void) { return s_ready && s_strip != NULL; }

static inline uint8_t scale8(uint8_t v, uint8_t b)
{
    return (uint16_t)v * b / 255;
}

int16_t matrix_xy_to_index(int16_t x, int16_t y)
{
    if (x < 0 || x >= LED_MATRIX_W || y < 0 || y >= LED_MATRIX_H) return -1;
    if ((y & 1) == 0) return (int16_t)(y * LED_MATRIX_W + x);
    return (int16_t)(y * LED_MATRIX_W + (LED_MATRIX_W - 1 - x));
}

void matrix_set_brightness(uint8_t b)
{
    if (b > MATRIX_MAX_BRIGHTNESS) b = MATRIX_MAX_BRIGHTNESS;
    s_bright = b;
}
uint8_t matrix_get_brightness(void) { return s_bright; }
void matrix_set_brightness_pct(uint8_t pct)
{
    if (pct < 1) pct = 1;
    if (pct > 100) pct = 100;
    uint8_t a = (uint8_t)((uint16_t)pct * MATRIX_MAX_BRIGHTNESS / 100u);
    matrix_set_brightness(a < 1 ? 1 : a);
}

void matrix_set_orient(uint8_t o)
{
    s_orient = (o == MATRIX_ORIENT_PORTRAIT) ? MATRIX_ORIENT_PORTRAIT
                                             : MATRIX_ORIENT_LANDSCAPE;
    s_fw = (s_orient == MATRIX_ORIENT_PORTRAIT) ? LED_MATRIX_H : LED_MATRIX_W;
    s_fh = (s_orient == MATRIX_ORIENT_PORTRAIT) ? LED_MATRIX_W : LED_MATRIX_H;
}
uint8_t matrix_get_orient(void) { return s_orient; }
uint8_t matrix_field_w(void) { return s_fw; }
uint8_t matrix_field_h(void) { return s_fh; }

/* portrait index: panel mounted vertical (10 cols x 12 rows).
   Portrait (x,y) <-> old (y, 9-x); serpentine of the fixed wiring. */
static int16_t matrix_xy_to_index_portrait(int16_t x, int16_t y)
{
    if (x < 0 || x >= LED_MATRIX_H || y < 0 || y >= LED_MATRIX_W) return -1;
    int16_t ox = y, oy = (int16_t)(9 - x);
    if ((oy & 1) == 0) return (int16_t)(oy * LED_MATRIX_W + ox);
    return (int16_t)(oy * LED_MATRIX_W + (LED_MATRIX_W - 1 - ox));
}
void matrix_set_idle(uint8_t m)
{
    s_idle = m > MATRIX_IDLE_OFF ? MATRIX_IDLE_FALAK : m;
}
uint8_t matrix_get_idle(void) { return s_idle; }

void matrix_set_xy(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b)
{
    int16_t i;
    if (s_orient == MATRIX_ORIENT_PORTRAIT)
        i = matrix_xy_to_index_portrait(x, y);
    else
        i = matrix_xy_to_index(x, y);
    if (i < 0) return;
    s_fb[i][0] = r; s_fb[i][1] = g; s_fb[i][2] = b;
}
void matrix_set_xy_rgb(int16_t x, int16_t y, rgb_t c)
{
    matrix_set_xy(x, y, c.r, c.g, c.b);
}
void matrix_clear(void)
{
    for (int i = 0; i < LED_MATRIX_COUNT; i++)
        s_fb[i][0] = s_fb[i][1] = s_fb[i][2] = 0;
}

void matrix_show(void)
{
    if (!s_ready || !s_strip) return;
    uint8_t eff = s_dim ? 10 : s_bright;   /* disconnect: 10% */
    if (!s_dim && s_dim_pct < 100)
        eff = (uint8_t)((uint16_t)eff * s_dim_pct / 100u);
    if (eff > MATRIX_MAX_BRIGHTNESS) eff = MATRIX_MAX_BRIGHTNESS;
    /* current limiter: estimate AFTER brightness, scale if > 1500 mA */
    uint32_t sum = 0;
    for (int i = 0; i < LED_MATRIX_COUNT; i++)
        sum += s_fb[i][0] + s_fb[i][1] + s_fb[i][2];
    uint32_t ma = sum * (uint32_t)eff * 20u / (255u * 255u);
    uint8_t lim = eff;
    if (ma > 1500 && ma > 0) lim = (uint8_t)((uint32_t)eff * 1500u / ma);
    for (int i = 0; i < LED_MATRIX_COUNT; i++) {
        led_strip_set_pixel(s_strip, (uint32_t)i,
            scale8(s_fb[i][0], lim),
            scale8(s_fb[i][1], lim),
            scale8(s_fb[i][2], lim));
    }
    led_strip_refresh(s_strip);
}

uint16_t matrix_estimate_ma(void)
{
    uint8_t eff = s_dim ? 10 : s_bright;
    if (eff > MATRIX_MAX_BRIGHTNESS) eff = MATRIX_MAX_BRIGHTNESS;
    uint32_t sum = 0;
    for (int i = 0; i < LED_MATRIX_COUNT; i++)
        sum += s_fb[i][0] + s_fb[i][1] + s_fb[i][2];
    uint32_t ma = sum * (uint32_t)eff * 20u / (255u * 255u);
    return ma > 0xFFFF ? 0xFFFF : (uint16_t)ma;
}

bool led_matrix_acquire(matrix_owner_t o)
{
    if (s_owner != OWNER_FALAK && o != s_owner) return false;
    s_owner = o;
    return true;
}
void led_matrix_release(matrix_owner_t o)
{
    if (s_owner == o) s_owner = OWNER_FALAK;
}
matrix_owner_t led_matrix_owner(void) { return s_owner; }
void matrix_set_dim_pct(uint8_t pct)
{
    s_dim_pct = pct > 100 ? 100 : pct;
}
/* short goodbye flash after arcade exit (visible even if idle == OFF) */
static uint32_t s_exit_until = 0;
void led_matrix_exit_flash(void)
{
    s_exit_until = (uint32_t)(esp_timer_get_time() / 1000) + 600;
}

void led_matrix_safe_off(void)
{
    matrix_clear();
    matrix_show();
}

void led_matrix_set_dim(bool dim) { s_dim = dim; }
void led_matrix_set_demo(bool on) { s_demo = on; }

/* ---------------- diagnostics ---------------- */
void matrix_test_rgb5(void)
{
    matrix_clear();
    /* Phase-A: pehle 5 LEDs R/G/B @20 brightness-raw (cap ke andar) */
    static const uint8_t cols[5][3] = {
        {20,0,0},{0,20,0},{0,0,20},{20,20,0},{20,0,20},
    };
    for (int x = 0; x < 5; x++)
        matrix_set_xy((int16_t)x, 0, cols[x][0], cols[x][1], cols[x][2]);
    matrix_show();
}

void matrix_test_border(void)
{
    matrix_clear();
    for (int x = 0; x < LED_MATRIX_W; x++) {
        matrix_set_xy_rgb(x, 0, COL_BORDER);
        matrix_set_xy_rgb(x, LED_MATRIX_H - 1, COL_BORDER);
    }
    for (int y = 0; y < LED_MATRIX_H; y++) {
        matrix_set_xy_rgb(0, y, COL_BORDER);
        matrix_set_xy_rgb(LED_MATRIX_W - 1, y, COL_BORDER);
    }
    /* corners gold taaki orientation dikhe: (0,0)=top-left */
    matrix_set_xy_rgb(0, 0, COL_GOLD);
    matrix_show();
}

void matrix_test_diagonal(void)
{
    matrix_clear();
    matrix_test_border();
    /* diagonal (0,0)->(9,9) mapper verify */
    for (int i = 0; i < 10; i++)
        matrix_set_xy_rgb(i, i, COL_CYAN);
    matrix_show();
}

/* ---------------- init / tick ---------------- */
void led_matrix_init(void)
{
    if (s_ready) return;
    matrix_clear();
    led_strip_config_t sc = {
        .strip_gpio_num = LED_MATRIX_GPIO,
        .max_leds = LED_MATRIX_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    /* 120 LEDs: non-DMA (rear strip owns DMA). Roof hata di hai isliye
       channel free hai. 48 symbols = roof jaisa footprint (S3 RMT mem
       4ch x 48 me fit); driver 120 LEDs ko chunk me bhejta hai. */
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .flags.with_dma = false,
    };
    esp_err_t e = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "matrix RMT FAILED gpio=%d err=%d (roof hata di? RMT free?)",
                 LED_MATRIX_GPIO, (int)e);
        s_strip = NULL;
        s_ready = false;
        return;
    }
    led_strip_clear(s_strip);
    led_strip_refresh(s_strip);
    s_ready = true;
    s_boot_t0 = (uint32_t)(esp_timer_get_time() / 1000);
    s_boot_step = 0;
    ESP_LOGI(TAG, "matrix init OK: 12x10=120 LEDs GPIO%d bright=%d (cap %d)",
             LED_MATRIX_GPIO, s_bright, MATRIX_MAX_BRIGHTNESS);
}

void led_matrix_deinit(void)
{
    if (s_strip) {
        led_strip_clear(s_strip);
        led_strip_refresh(s_strip);
        led_strip_del(s_strip);
        s_strip = NULL;
    }
    s_ready = false;
}

/* Boot self-test (Phase-A/B) + idle sweep. 30 FPS throttle. */
void led_matrix_tick(uint32_t now_ms)
{
    if (!s_ready || !s_demo) return;
    if (now_ms - s_last_show < 33) return;   /* max ~30 FPS */
    s_last_show = now_ms;

    uint32_t el = now_ms - s_boot_t0;
    if (el < 1000) {
        /* 0-1s: Phase-A 5-LED RGB test */
        if (s_boot_step != 1) { s_boot_step = 1; matrix_test_rgb5(); }
        return;
    }
    if (el < 2000) {
        /* 1-2s: border (zig-zag check) */
        if (s_boot_step != 2) { s_boot_step = 2; matrix_test_border(); }
        return;
    }
    if (el < 3000) {
        /* 2-3s: diagonal mapper check, then idle */
        if (s_boot_step != 3) { s_boot_step = 3; matrix_test_diagonal(); }
        return;
    }
    /* uske baad: idle show setting (FALAK / SWEEP / OFF) */
    s_boot_step = 4;
    if (s_owner != OWNER_FALAK) return;   /* arcade owns the matrix now */
    {
        uint32_t tnow = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(s_exit_until - tnow) > 0) {
            /* exit flash: 3 gold blinks so OFF-idle exits are visible */
            matrix_clear();
            if (((tnow / 200) & 1) == 0) {
                for (int x = 0; x < s_fw; x += 2)
                    matrix_set_xy_rgb(x, s_fh / 2, COL_GOLD);
            }
            matrix_show();
            return;
        }
    }
    if (s_idle == MATRIX_IDLE_SWEEP) {
        /* classic gold sweep (test loop), follows field size */
        matrix_clear();
        for (int x = 0; x < s_fw; x++) {
            matrix_set_xy_rgb(x, 0, COL_BORDER);
            matrix_set_xy_rgb(x, s_fh - 1, COL_BORDER);
        }
        for (int y = 0; y < s_fh; y++) {
            matrix_set_xy_rgb(0, y, COL_BORDER);
            matrix_set_xy_rgb(s_fw - 1, y, COL_BORDER);
        }
        int sx = (int)((now_ms / 150) % s_fw);
        matrix_set_xy_rgb(sx, 0, COL_GOLD);
        matrix_show();
    } else if (s_idle == MATRIX_IDLE_FALAK) {
        falak_tick(now_ms);
    } else {
        matrix_clear();
        matrix_show();
    }
    if (s_dim) {
        /* disconnect: slow red border pulse on top (blueprint 13.7) */
        bool on = ((now_ms / 500) & 1) == 0;
        rgb_t c = on ? COL_RED : COL_OFF;
        for (int x = 0; x < s_fw; x++) {
            matrix_set_xy_rgb(x, 0, c);
            matrix_set_xy_rgb(x, s_fh - 1, c);
        }
        for (int y = 0; y < s_fh; y++) {
            matrix_set_xy_rgb(0, y, c);
            matrix_set_xy_rgb(s_fw - 1, y, c);
        }
        matrix_show();
    }
}
