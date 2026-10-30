/* c3_receiver_s3.c - ESP-NOW receiver for the ESP32-C3 handheld node.
 * Runs as a FreeRTOS task; receives telemetry broadcasts and updates
 * the global c3_link_state. Also decodes incoming ADPCM megaphone audio.
 */
#include "c3_receiver_s3.h"
#include "c3_wireless.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_timer.h"

#include "car.h"          /* car_i2s0_handle, car_audio_pause */
#include "car_global.h"

static const char *TAG = "c3_rx";

static c3_link_state_t s_state;
static uint32_t s_last_seq = 0;
static bool s_have_seq = false;

/* ---- ESP-NOW receive callback (runs in WiFi task context) ---- */
static void c3_on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!data || len < (int)sizeof(c3_telemetry_t)) return;
    const c3_telemetry_t *pkt = (const c3_telemetry_t *)data;
    if (pkt->magic != C3_TELEMETRY_MAGIC) return;

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    s_state.last_rx_ms = now;
    s_state.connected = true;
    s_state.rssi = info ? (uint8_t)(info->rx_ctrl ? (info->rx_ctrl->rssi + 128) / 2 : 0) : 0;
    if (info ? (info->rx_ctrl != NULL) : false) { (void)info; }

    memcpy(&s_state.last, pkt, sizeof(c3_telemetry_t));
    s_state.seq = pkt->seq;
    s_state.packets_rx++;

    /* estimate lost packets via seq gaps */
    if (s_have_seq) {
        uint8_t gap = (uint8_t)(pkt->seq - s_last_seq);
        if (gap > 1) s_state.packets_lost += (gap - 1);
    }
    s_last_seq = pkt->seq;
    s_have_seq = true;
}

/* ensure c3_link_state_t has src_mac? It doesn't. Remove above memcpy reliance. */

const c3_link_state_t *c3_link_state(void) { return &s_state; }
bool c3_link_connected(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    if (!s_state.connected) return false;
    if ((now - s_state.last_rx_ms) > 500) { s_state.connected = false; return false; }
    return true;
}

/* ---- WiFi init for ESP-NOW (STA mode, channel 1) ---- */
static esp_err_t c3_wifi_init(void)
{
    /* WiFi may already be initialised/started by net_wifi. Handle gracefully. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* ok */ }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* ok */ }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* ok */ }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_wifi_set_channel(C3_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_channel: %s", esp_err_to_name(err));
    return ESP_OK;
}


void c3_receiver_init(void)
{
    memset(&s_state, 0, sizeof(s_state));
    c3_wifi_init();
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return;
    }
    esp_now_register_recv_cb(c3_on_recv);
    ESP_LOGI(TAG, "C3 receiver ready on channel %d", C3_ESPNOW_CHANNEL);
}

void c3_receiver_task(void *arg)
{
    c3_receiver_init();
    /* periodic connection watchdog */
    while (1) {
        c3_link_connected(); /* updates connected flag */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- Megaphone PA: IMA-ADPCM decode + playback ---- */
/* Minimal IMA-ADPCM decoder (mono). */
static const int16_t s_adpcm_step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t s_adpcm_index_table[16] = { -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8 };

static int s_adpcm_pred = 0;
static int s_adpcm_idx = 0;

static int16_t adpcm_decode_nibble(uint8_t n)
{
    int step = s_adpcm_step_table[s_adpcm_idx];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    if (n & 8) diff = -diff;
    s_adpcm_pred += diff;
    if (s_adpcm_pred > 32767) s_adpcm_pred = 32767;
    if (s_adpcm_pred < -32768) s_adpcm_pred = -32768;
    s_adpcm_idx += s_adpcm_index_table[n & 0x0F];
    if (s_adpcm_idx < 0) s_adpcm_idx = 0;
    if (s_adpcm_idx > 88) s_adpcm_idx = 88;
    return (int16_t)s_adpcm_pred;
}

void c3_megaphone_feed(const uint8_t *adpcm, uint32_t len_bytes, uint8_t vol, uint8_t effect)
{
    if (!adpcm || len_bytes == 0) return;
    car_audio_pause(true);
    /* decode 4-bit ADPCM -> 16-bit PCM, resample 16k->22050 (nearest) */
    static int16_t pcm[512];
    uint32_t pcm_n = 0;
    for (uint32_t i = 0; i < len_bytes && pcm_n + 2 <= 512; i++) {
        uint8_t b = adpcm[i];
        pcm[pcm_n++] = adpcm_decode_nibble(b & 0x0F);
        pcm[pcm_n++] = adpcm_decode_nibble((b >> 4) & 0x0F);
    }
    /* apply volume */
    for (uint32_t i = 0; i < pcm_n; i++) pcm[i] = (int16_t)(pcm[i] * vol / 100);
    /* effect: 0 clean, 1 robot (bitcrush), 2 monster (downpitch via skip), 3 radio (bandpass-ish clip) */
    if (effect == 1) { for (uint32_t i = 0; i < pcm_n; i++) pcm[i] = (pcm[i] >> 4) << 4; }
    if (effect == 3) { for (uint32_t i = 0; i < pcm_n; i++) { if (pcm[i] > 8000) pcm[i]=8000; if (pcm[i] < -8000) pcm[i]=-8000; } }
    car_audio_write_pcm(pcm, (int)pcm_n);
    car_audio_pause(false);
}

void c3_megaphone_stop(void)
{
    car_audio_pause(false);
}
