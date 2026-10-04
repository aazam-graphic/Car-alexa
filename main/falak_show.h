/*
 * falak_show.h - FALAK name + beating heart show for 120-LED matrix.
 * Idle screen: beating heart with ECG pulse line, then scrolling FALAK.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Every matrix loop call (already ~30 FPS throttled by caller). */
void falak_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
