/* SPDX-License-Identifier: Apache-2.0 */
/* net_wifi.h - generic Wi-Fi station + SNTP time sync (no chatbot dependency).
 *
 * Alexa bridge (MQTT/TLS) needs: (1) station IP, (2) valid wall-clock for
 * certificate validation + command TTL. Both are managed here, non-blocking.
 * Credentials: NVS namespace "net" keys wifi_ssid/wifi_pass, else defaults.
 */
#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_DOWN = 0,      /* not started */
    NET_WIFI_CONNECTING,    /* connecting / retrying */
    NET_WIFI_UP,            /* connected, has IP */
    NET_WIFI_OFF,           /* deliberately off after time sync (power save) */
} net_wifi_state_t;

/* Default credentials - override via NVS keys "wifi_ssid" / "wifi_pass"
 * in namespace "net". (Keep public defaults empty; set yours via NVS.) */
#define NET_WIFI_DEFAULT_SSID  ""
#define NET_WIFI_DEFAULT_PASS  ""

/* Start station mode + connect. Idempotent, non-blocking (event-driven). */
esp_err_t net_wifi_start(void);

net_wifi_state_t net_wifi_get_state(void);
bool             net_wifi_up(void);
/* True once SNTP has set a valid epoch (needed for MQTT TLS + cmd TTL). */
bool             net_wifi_time_synced(void);
/* Last known epoch (NVS, saved every 60 s): instant clock at boot before
   SNTP locks. 0 = never synced yet. */
time_t           net_wifi_last_known(void);
/* Call every car_task loop: periodic epoch save + WiFi auto-off 5 s after
   sync (or 90 s give-up). Zero work otherwise. */
void             net_wifi_poll(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
