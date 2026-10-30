/* c3_wireless.h - shared ESP-NOW protocol between ESP32-C3 handheld node
 * and ESP32-S3 car mainframe. Freestanding header. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C3_ESPNOW_CHANNEL   1
#define C3_SPEC_BANDS       23
#define C3_NODE_MAC_LEN     6

#define C3_GEST_NONE         0x00
#define C3_GEST_FLIP_ESTOP   0x01
#define C3_GEST_JERK_UP      0x02
#define C3_GEST_DBL_SHAKE    0x04
#define C3_GEST_STILL_8S     0x08

#define C3_MIC_EVT_NONE      0x00
#define C3_MIC_EVT_CLAP2     0x01
#define C3_MIC_EVT_CLAP3     0x02
#define C3_MIC_EVT_CLAP4     0x04
#define C3_MIC_EVT_BLOW      0x08
#define C3_MIC_EVT_WHISTLE   0x10
#define C3_MIC_EVT_WIND      0x20
#define C3_MIC_EVT_VOICE     0x40

typedef enum {
    C3_VOICE_NONE = 0,
    C3_VOICE_HEADLIGHT_ON,
    C3_VOICE_HEADLIGHT_OFF,
    C3_VOICE_HORN,
    C3_VOICE_TURBO,
    C3_VOICE_ESTOP,
    C3_VOICE_FIND_CAR,
    C3_VOICE_MATRIX_FIRE,
    C3_VOICE_COUNT
} c3_voice_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic;
    uint8_t  seq;
    uint8_t  gestures;
    uint8_t  mic_evt;
    uint8_t  voice_cmd;
    int8_t   roll_deg;
    int8_t   pitch_deg;
    int16_t  yaw_deg_x10;
    int16_t  lin_g_x100;
    uint8_t  tilt_steer;
    uint8_t  spectrum[C3_SPEC_BANDS];
    uint8_t  db_level;
    uint8_t  battery_pct;
    uint32_t ts_ms;
} c3_telemetry_t;

#define C3_TELEMETRY_MAGIC 0xC3

typedef struct {
    bool     connected;
    uint32_t last_rx_ms;
    uint8_t  rssi;
    uint8_t  seq;
    c3_telemetry_t last;
    uint32_t packets_rx;
    uint32_t packets_lost;
} c3_link_state_t;

void c3_receiver_init(void);
void c3_receiver_task(void *arg);
const c3_link_state_t *c3_link_state(void);
bool c3_link_connected(void);

#ifdef C3_NODE_BUILD
bool c3_send_telemetry(const c3_telemetry_t *pkt);
#endif

#define C3_ADPCM_SAMPLE_RATE  16000
#define C3_ADPCM_FRAME_BYTES  128

typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t flags;
    uint8_t vol;
    uint8_t effect;
} c3_audio_ctrl_t;
#define C3_AUDIO_CTRL_MAGIC 0xA3

#ifdef __cplusplus
}
#endif
