/* c3_receiver_s3.h - ESP-NOW receiver for the ESP32-C3 handheld node. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "c3_wireless.h"

#ifdef __cplusplus
extern "C" {
#endif

void c3_receiver_init(void);
void c3_receiver_task(void *arg);
const c3_link_state_t *c3_link_state(void);
bool c3_link_connected(void);

/* Megaphone PA: feed 4-bit ADPCM bytes; decodes and plays on speaker.
 * Caller must pause engine audio around this. */
void c3_megaphone_feed(const uint8_t *adpcm, uint32_t len_bytes, uint8_t vol, uint8_t effect);
void c3_megaphone_stop(void);

#ifdef __cplusplus
}
#endif
