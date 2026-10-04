/* SPDX-License-Identifier: Apache-2.0 */
/* net_wifi.c - Wi-Fi station management + SNTP (Alexa cloud link ke liye).
 * Safe: never blocks car_task; state is a volatile enum read by UI/tasks. */
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "net_wifi.h"

static const char *TAG = "net_wifi";

#define NET_CONNECT_BIT BIT0
#define NET_MAX_RETRY   6

static EventGroupHandle_t s_evt;
static int s_retry = 0;
static char s_ssid[33] = NET_WIFI_DEFAULT_SSID;
static char s_pass[65] = NET_WIFI_DEFAULT_PASS;
static volatile net_wifi_state_t s_state = NET_WIFI_DOWN;
static volatile bool s_time_ok = false;
static bool s_started = false;
static time_t s_last_known = 0;      /* NVS last epoch: instant boot clock */
static uint32_t s_sync_ms = 0;       /* SNTP lock timestamp (for wifi-off) */
static uint32_t s_start_ms = 0;      /* start timestamp (90 s give-up) */
static uint32_t s_last_save_ms = 0;  /* 60 s epoch save cadence */
static bool s_wifi_off = false;

static uint32_t wifi_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* persist epoch (NVS "net"/"last_t") */
static void save_epoch(time_t t)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, "last_t", (uint64_t)t);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Load SSID/Pass from NVS if present (fallback = defaults above). */
static void load_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
        size_t l = sizeof(s_ssid);
        if (nvs_get_str(h, "wifi_ssid", s_ssid, &l) != ESP_OK) {
            strncpy(s_ssid, NET_WIFI_DEFAULT_SSID, sizeof(s_ssid));
        }
        l = sizeof(s_pass);
        if (nvs_get_str(h, "wifi_pass", s_pass, &l) != ESP_OK) {
            strncpy(s_pass, NET_WIFI_DEFAULT_PASS, sizeof(s_pass));
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "using SSID %s", s_ssid);
}

static void handle_connect(void)
{
    s_retry = 0;
    s_state = NET_WIFI_UP;
    xEventGroupSetBits(s_evt, NET_CONNECT_BIT);
    ESP_LOGI(TAG, "connected, IP assigned");
}

/* Timer callback: runs on esp_timer task - just kick a connect. */
static void reconnect_timer_cb(void *arg)
{
    esp_wifi_connect();
    (void)arg;
}

static void handle_disconnect(void)
{
    s_state = NET_WIFI_CONNECTING;
    s_retry++;
    /* Retry karte rehna - kabhi give up nahi. Pehle 6 tez (2s), phir
       slow (6s) taaki WiFi hamesha connect rahe. */
    uint64_t delay_us = (s_retry <= NET_MAX_RETRY) ? 2000000ULL : 6000000ULL;
    /* one reusable timer: a flap must never leak a timer per disconnect */
    static esp_timer_handle_t s_retry_t = NULL;
    if (s_retry_t) {
        esp_timer_stop(s_retry_t);
        esp_timer_delete(s_retry_t);
        s_retry_t = NULL;
    }
    const esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb, .name = "netwifi_rc",
        .dispatch_method = ESP_TIMER_TASK,
    };
    if (esp_timer_create(&targs, &s_retry_t) == ESP_OK) {
        esp_timer_start_once(s_retry_t, delay_us);
    } else {
        esp_wifi_connect();                          /* fallback: immediate */
    }
    ESP_LOGW(TAG, "reconnecting in %.1fs (retry %d)", delay_us / 1e6, s_retry);
    if (s_retry > NET_MAX_RETRY) s_retry = NET_MAX_RETRY;
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_ok = true;
    time_t t = time(NULL);
    ESP_LOGI(TAG, "SNTP synced, epoch=%lld", (long long)t);
    /* instant next boot + 60 s cadence anchor */
    s_last_known = t;
    save_epoch(t);
    if (!s_sync_ms) s_sync_ms = wifi_now_ms();
    s_last_save_ms = wifi_now_ms();
}

static void start_sntp(void)
{
    /* India Standard Time: UTC+5:30, no DST — required before any
       localtime_r() so the standby clock reads IST, not UTC. */
    setenv("TZ", "IST-5:30", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = time_sync_cb;
    esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "sntp init err %s", esp_err_to_name(e));
    }
    /* NOTE: lwIP SNTP default re-syncs hourly in background (drift
       correction) — no extra timer needed. */
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            handle_disconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        handle_connect();
    }
}

esp_err_t net_wifi_start(void)
{
    if (s_started) return ESP_OK;

    /* NVS FIRST (Oct 2026 bugfix): load_creds() needs NVS up, otherwise
       stored SSID/password silently fall back to defaults. */
    esp_err_t er = nvs_flash_init();
    if (er == ESP_ERR_NVS_NO_FREE_PAGES || er == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        er = nvs_flash_init();
    }
    if (er != ESP_OK) return er;
    load_creds();

    s_evt = xEventGroupCreate();
    if (!s_evt) return ESP_ERR_NO_MEM;
    s_state = NET_WIFI_CONNECTING;

    er = esp_netif_init();
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) return er;
    er = esp_event_loop_create_default();
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) return er;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    er = esp_wifi_init(&icfg);
    if (er != ESP_OK && er != ESP_ERR_INVALID_STATE) return er;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     s_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();
    start_sntp();
    s_started = true;
    s_start_ms = wifi_now_ms();
    /* last known epoch for the instant boot clock */
    {
        nvs_handle_t h;
        if (nvs_open("net", NVS_READONLY, &h) == ESP_OK) {
            uint64_t lt = 0;
            if (nvs_get_u64(h, "last_t", &lt) == ESP_OK && lt > 1700000000ULL)
                s_last_known = (time_t)lt;
            nvs_close(h);
        }
        if (s_last_known)
            ESP_LOGI(TAG, "last known epoch=%lld (instant clock)",
                     (long long)s_last_known);
    }
    return ESP_OK;
}

net_wifi_state_t net_wifi_get_state(void) { return s_state; }
bool net_wifi_up(void)                    { return s_state == NET_WIFI_UP; }
bool net_wifi_time_synced(void)           { return s_time_ok; }
time_t net_wifi_last_known(void)          { return s_last_known; }

void net_wifi_poll(uint32_t now_ms)
{
    if (!s_started || s_wifi_off) return;
    /* one-time: persist working creds to NVS (car_task context) */
    static bool s_cred_done = false;
    if (!s_cred_done && s_state == NET_WIFI_UP) {
        s_cred_done = true;
        nvs_handle_t h;
        esp_err_t oe = nvs_open("net", NVS_READWRITE, &h);
        if (oe != ESP_OK) {
            ESP_LOGW(TAG, "cred save: nvs_open=%d", (int)oe);
        } else {
            char cur[33] = {0};
            size_t l = sizeof(cur);
            bool need = (nvs_get_str(h, "wifi_ssid", cur, &l) != ESP_OK) ||
                        strcmp(cur, s_ssid) != 0;
            if (need) {
                nvs_set_str(h, "wifi_ssid", s_ssid);
                nvs_set_str(h, "wifi_pass", s_pass);
                esp_err_t ce = nvs_commit(h);
                ESP_LOGI(TAG, "WiFi creds saved to NVS (commit=%d)", (int)ce);
            } else {
                ESP_LOGI(TAG, "WiFi creds already in NVS");
            }
            nvs_close(h);
        }
    }
    /* 60 s epoch save cadence (crash-safe last-known) */
    if (s_time_ok && now_ms - s_last_save_ms >= 60000) {
        s_last_save_ms = now_ms;
        time_t t = time(NULL);
        if (t > 1700000000L) {
            s_last_known = t;
            save_epoch(t);
        }
    }
    /* WiFi auto-off: 5 s after sync, or 90 s give-up (power save) */
    bool synced = s_time_ok && s_sync_ms && now_ms - s_sync_ms >= 5000;
    bool giveup = !s_time_ok && s_start_ms && now_ms - s_start_ms >= 90000;
    if (synced || giveup) {
        s_wifi_off = true;
        esp_wifi_stop();
        s_state = NET_WIFI_OFF;
        ESP_LOGI(TAG, "WiFi OFF (%s)", synced ? "time synced" : "sync timeout");
    }
}
