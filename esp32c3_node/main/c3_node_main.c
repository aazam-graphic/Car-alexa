#define C3_NODE_BUILD
#include "c3_wireless.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "c3_node";

#define MPU_SDA 4
#define MPU_SCL 5
#define MIC_SCK 6
#define MIC_WS  7
#define MIC_SD  8
#define PTT_PIN 9

#define MPU_ADDR 0x68
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_mpu_dev;

static esp_err_t mpu_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_mpu_dev, buf, 2, 100);
}

static esp_err_t mpu_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_mpu_dev, &reg, 1, buf, len, 100);
}

static bool mpu_init(void)
{
    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = MPU_SDA,
        .scl_io_num = MPU_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &s_i2c_bus) != ESP_OK) return false;
    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(s_i2c_bus, &dcfg, &s_mpu_dev) != ESP_OK) return false;
    mpu_write(0x6B, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    mpu_write(0x1C, 0x00);
    mpu_write(0x1B, 0x00);
    mpu_write(0x1A, 0x03);
    mpu_write(0x19, 19);
    return true;
}

static void mpu_read_ag(float *ax, float *ay, float *az, float *gx, float *gy, float *gz)
{
    uint8_t buf[14];
    if (mpu_read(0x3B, buf, 14) != ESP_OK) return;
    int16_t raw_ax = (buf[0]<<8)|buf[1];
    int16_t raw_ay = (buf[2]<<8)|buf[3];
    int16_t raw_az = (buf[4]<<8)|buf[5];
    int16_t raw_gx = (buf[8]<<8)|buf[9];
    int16_t raw_gy = (buf[10]<<8)|buf[11];
    int16_t raw_gz = (buf[12]<<8)|buf[13];
    *ax = raw_ax / 16384.0f;
    *ay = raw_ay / 16384.0f;
    *az = raw_az / 16384.0f;
    *gx = raw_gx / 131.0f;
    *gy = raw_gy / 131.0f;
    *gz = raw_gz / 131.0f;
}

static i2s_chan_handle_t s_mic_chan;
static bool mic_init(void)
{
    i2s_chan_config_t cchan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    if (i2s_new_channel(&cchan, NULL, &s_mic_chan) != ESP_OK) return false;
    i2s_std_config_t scfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK,
            .ws = MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = MIC_SD,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(s_mic_chan, &scfg) != ESP_OK) return false;
    i2s_channel_enable(s_mic_chan);
    return true;
}

static esp_err_t wifi_init(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(C3_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    return ESP_OK;
}

static bool espnow_init(void)
{
    if (wifi_init() != ESP_OK) return false;
    if (esp_now_init() != ESP_OK) return false;
    return true;
}

bool c3_send_telemetry(const c3_telemetry_t *pkt)
{
    static uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    return esp_now_send(broadcast_mac, (const uint8_t *)pkt, sizeof(c3_telemetry_t)) == ESP_OK;
}
static float s_roll = 0, s_pitch = 0, s_yaw = 0;
static uint8_t s_seq = 0;
static uint32_t s_last_move_ms = 0;
static bool s_still_reported = false;
static uint8_t s_spectrum[C3_SPEC_BANDS];

static void compute_spectrum(const int16_t *samples, size_t n)
{
    static const float freqs[C3_SPEC_BANDS] = {
        100,140,196,275,385,540,756,1058,1482,2075,2905,4067,5694,
        6500,6900,7200,7500,7800,8000,8200,8400,8600,8800
    };
    for (int b = 0; b < C3_SPEC_BANDS; b++) {
        float w = 2.0f * (float)M_PI * freqs[b] / 16000.0f;
        float coeff = 2.0f * cosf(w);
        float s1 = 0, s2 = 0;
        for (size_t i = 0; i < n; i++) {
            float x = samples[i] / 32768.0f;
            float s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float power = s1*s1 + s2*s2 - coeff*s1*s2;
        if (power < 0) power = 0;
        float mag = sqrtf(power) / (n/2.0f);
        int v = (int)(mag * 400.0f);
        if (v > 255) v = 255;
        if (v < 0) v = 0;
        s_spectrum[b] = (uint8_t)v;
    }
}

static int s_clap_count = 0;
static uint32_t s_clap_window_start = 0;
static uint32_t s_last_clap_ms = 0;
static uint8_t s_mic_evt = 0;

static void detect_claps(uint16_t rms, uint32_t now_ms)
{
    static uint16_t s_prev_rms = 0;
    if (rms > 6000 && s_prev_rms < 2000) {
        if (now_ms - s_last_clap_ms > 150) {
            s_clap_count++;
            s_last_clap_ms = now_ms;
            if (s_clap_window_start == 0) s_clap_window_start = now_ms;
        }
    }
    s_prev_rms = rms;
}

static void finalize_claps(void)
{
    if (s_clap_count == 2) s_mic_evt |= C3_MIC_EVT_CLAP2;
    else if (s_clap_count == 3) s_mic_evt |= C3_MIC_EVT_CLAP3;
    else if (s_clap_count >= 4) s_mic_evt |= C3_MIC_EVT_CLAP4;
    s_clap_count = 0;
    s_clap_window_start = 0;
}

static void detect_blow(const int16_t *samples, size_t n, uint16_t rms)
{
    uint32_t lf = 0;
    for (int b = 0; b < 4; b++) lf += s_spectrum[b];
    if (rms > 4000 && lf > 600) {
        static uint32_t blow_start = 0;
        if (blow_start == 0) blow_start = (uint32_t)(esp_timer_get_time()/1000ULL);
        else if ((uint32_t)(esp_timer_get_time()/1000ULL) - blow_start > 300) {
            s_mic_evt |= C3_MIC_EVT_BLOW;
            blow_start = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "C3 node starting");
    nvs_flash_init();
    mpu_init();
    mic_init();
    espnow_init();

    float ax,ay,az,gx,gy,gz;
    float gx_bias=0,gy_bias=0,gz_bias=0;
    for (int i = 0; i < 200; i++) {
        mpu_read_ag(&ax,&ay,&az,&gx,&gy,&gz);
        gx_bias += gx; gy_bias += gy; gz_bias += gz;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    gx_bias /= 200; gy_bias /= 200; gz_bias /= 200;

    gpio_config_t ptt = {
        .pin_bit_mask = (1ULL << PTT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ptt);

    uint32_t last_send = 0;
    uint32_t last_clap_finalize = 0;
    int16_t mic_buf[512];

    while (1) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        mpu_read_ag(&ax,&ay,&az,&gx,&gy,&gz);
        gx -= gx_bias; gy -= gy_bias; gz -= gz_bias;

        float roll_acc = atan2f(ay, az) * 57.2958f;
        float pitch_acc = atan2f(-ax, sqrtf(ay*ay + az*az)) * 57.2958f;
        s_roll  = 0.96f * (s_roll  + gx * 0.02f) + 0.04f * roll_acc;
        s_pitch = 0.96f * (s_pitch + gy * 0.02f) + 0.04f * pitch_acc;
        s_yaw += gz * 0.02f;

        uint8_t gest = C3_GEST_NONE;
        if (az < -0.7f) gest |= C3_GEST_FLIP_ESTOP;
        if (az > 1.6f) gest |= C3_GEST_JERK_UP;
        float mov = fabsf(gx)+fabsf(gy)+fabsf(gz);
        if (mov < 2.0f) {
            if (s_last_move_ms == 0) s_last_move_ms = now_ms;
            if (!s_still_reported && (now_ms - s_last_move_ms) > 8000) {
                gest |= C3_GEST_STILL_8S;
                s_still_reported = true;
            }
        } else {
            s_last_move_ms = now_ms;
            s_still_reported = false;
        }

        size_t bytes_read = 0;
        i2s_channel_read(s_mic_chan, mic_buf, sizeof(mic_buf), &bytes_read, 10);
        size_t nsamp = bytes_read / 2;
        uint32_t sq = 0;
        for (size_t i = 0; i < nsamp; i++) sq += (uint32_t)(mic_buf[i] * mic_buf[i]);
        uint16_t rms = nsamp ? (uint16_t)sqrtf((float)sq / nsamp) : 0;
        if (nsamp >= 256) compute_spectrum(mic_buf, nsamp > 512 ? 512 : nsamp);
        uint8_t db = (uint8_t)(rms / 327);
        if (db > 100) db = 100;

        detect_claps(rms, now_ms);
        if (s_clap_window_start && (now_ms - s_clap_window_start) > 1200) {
            if (now_ms - last_clap_finalize > 1300) {
                finalize_claps();
                last_clap_finalize = now_ms;
            }
        }
        detect_blow(mic_buf, nsamp, rms);

        if ((fabsf(gx) + fabsf(gy)) > 300.0f) {
            gest |= C3_GEST_DBL_SHAKE;
        }

        int rs = (int)((s_roll + 45.0f) * (255.0f / 90.0f));
        if (rs < 0) rs = 0;
        if (rs > 255) rs = 255;

        if (now_ms - last_send >= 20) {
            c3_telemetry_t pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.magic = C3_TELEMETRY_MAGIC;
            pkt.seq = s_seq++;
            pkt.gestures = gest;
            pkt.mic_evt = s_mic_evt;
            s_mic_evt = 0;
            pkt.voice_cmd = C3_VOICE_NONE;
            pkt.roll_deg = (int8_t)s_roll;
            pkt.pitch_deg = (int8_t)s_pitch;
            pkt.yaw_deg_x10 = (int16_t)(s_yaw * 10.0f);
            float amag = sqrtf(ax*ax+ay*ay+az*az);
            pkt.lin_g_x100 = (int16_t)((amag - 1.0f) * 100.0f);
            pkt.tilt_steer = (uint8_t)rs;
            memcpy(pkt.spectrum, s_spectrum, C3_SPEC_BANDS);
            pkt.db_level = db;
            pkt.battery_pct = 100;
            pkt.ts_ms = now_ms;
            c3_send_telemetry(&pkt);
            last_send = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

