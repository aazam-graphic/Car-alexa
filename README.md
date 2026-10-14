# AZAM CAR OS + 23x13 Matrix Arcade — Updated Project Details

> Last updated: Oct 2026 — verified against current source/build logs.

## Ek line mein
Xbox 360 wireless gamepad se chalne wala robot car + Car OS TFT dashboard + 23x13 physical RGB matrix arcade.

## Status / changes
- **Alexa/cloud fully removed** (bridge/config/task/UI/deps; remaining cloud/Alexa assets removed).
- **OLED removed** from hardware and runtime UI.
- **MPU disabled** with `MPU_HW_PRESENT 0` (code retained behind guards; no hardware expected).
- **Matrix hardware updated:** physical WS2812 chain is **299 LEDs**, logical field **23 columns x 13 rows**, custom `main/matrix_layout_23x13_299.h` map. Buffer index = drawing label - 1.
- **NEON SERPENT V2** active: 20 levels + Endless, smart reachable food, gold/prism food, power-ups, hunter/patrol/mines, TFT companion.
- **Instant clock:** before SNTP, last-known epoch from NVS flashes/appears; WiFi syncs, then shuts down after sync/timeout.
- **Self-test at boot:** logs `selftest done: 15 PASS 0 FAIL`.

## Hardware
| Item | Detail |
|---|---|
| MCU | ESP32-S3 N16R8, 16 MB Flash, 8 MB Octal PSRAM |
| Framework | ESP-IDF v5.5.5 |
| Controller | Xbox 360 Wireless (Redgear Pro dongle), USB-OTG GPIO19/20 |
| TFT | 320x240 RGB565 SPI, GPIO21/38/45/0/2/etc depending on existing config |
| Matrix | 23x13, 299 WS2812, DIN GPIO18, non-DMA RMT, brightness cap 72/255, 1500mA limiter |
| Motors | D1 slave UART TX GPIO1 |
| Servos | PAN GPIO4, TILT GPIO5 |
| Distances | HC-SR04 x3 |
| Audio | MAX98357A I2S |
| Mic | INMP441-class I2S |
| Removed | OLED SSD1306, MPU I2C expected hardware, rear US/gripper/spoiler if previously removed |

## Matrix layout
`main/matrix_layout_23x13_299.h` maps visual `(x,y)` to physical WS2812 index. Games draw by logical coordinates only: `matrix_set_xy(x,y,r,g,b)` / `matrix_set_xy_rgb`, `matrix_show`.

## Matrix Show (299 LED, 20 effects)
- Entry: OS screen `MATRIX SHOW` (row on HOME). LB/RB or D-pad left/right = effect, X = random, A/START = auto-demo toggle, B/BACK = exit, GUIDE stays reserved for E-STOP.
- **Pacing:** dedicated FreeRTOS task `m_show` (prio 3, 4096 stack) drives frames at `SHOW_FRAME_MS = 30` → **33 FPS**, decoupled from car-loop jitter. The car loop still owns all input/state and calls the same frame path; the frame gate lives inside the `s_fb_lock` mutex so a 30 ms step can never be simulated twice.
- **Afterglow:** `s_ghost[13][23][3]` phosphor buffer, `out = max(new, prev * 168/255)`, values under 3 clamp to 0 (~4 frame tail). Gives moving LEDs a real trail instead of 1-pixel snapping. Applies to every effect.
- **Fireworks** is a particle sim in `main/matrix_fx.c` (not a per-pixel ring):
  - fixed pool of 56 particles, no malloc, xorshift RNG reseeded per cycle → deterministic
  - physics per 30 ms step: drag (x 14/256, y 11/256), gravity +5/256 → willow droop, off-field cull
  - 6 bursts per 2700 ms cycle at 0/420/900/1320/1680/2280 ms. **Offsets must stay multiples of `FX_STEP_MS`**, otherwise a burst is silently skipped (the sim only ever observes 0,30,60,…).
  - 3 particle kinds: SPARK (radial), WILL (long-lived droop), SNAKE (hue-cycling strobe)
  - rendered with **bilinear sub-pixel splats + additive blending** (`matrix_add_xy`) → motion is continuous, not quantized to whole pixels
  - trail = 2 sub-frame samples behind the head, strength gated by speed so a stationary ember does not stack into a saturated blob
  - `FX_IGNITE = 3` ramp on launch (a real shell takes ~100 ms to reach full brightness; an instant 0→255 pop is the "sharp" artefact)
  - night-sky background: vertical gradient + hashed twinkling stars
  - burst launch plays a low `car_sfx_blip()` thump (pitch alternates); no new audio assets
- Host-sim measurements over a 90-frame cycle (vs the old procedural ring): hard pixel snaps (Δ≥100 in one frame) **844 → 345 (-59%)**, avg lit LEDs 184/299, **0 black frames** (old design had ~700 ms dark per cycle).
- New driver helpers: `matrix_add_xy()` (saturating additive) and `matrix_get_xy()` (logical read-back for the afterglow pass). Both go through the same mapper — the physical map is still defined in exactly one place.

## NEON SERPENT V2 states / controls
- MENU: level select via Y/X up, A/B down; START play; LB/RB rumble OFF/SOFT/FULL.
- PLAY: Y=UP, A=DOWN, X=LEFT, B=RIGHT; START pause; BACK menu; disconnect pause.
- BACK hold: exit arcade. GUIDE/E-STOP exits too.

## Current snake visuals
- Snake: stable segments, head steady (no eat-burst).
- Mine: one solid red pixel, steady.
- Patrol: magenta strobe, one pixel.
- Hunter: orange strobe, one pixel.
- Normal food: warm orange one pixel, breathing brightness.
- Gold food: yellow/gold one pixel.
- Prism food: color-cycled one pixel.
- Power-up: one pixel, color by type.
- Level 4 border: soft teal, slightly brighter.

## TFT companion
NEON SERPENT V2 screen: live mirror, score/hi, level/name, food progress, lives, wrap mode, mines, run/level power banner, stats row (`LEN`, `COMBO`, `TIME`), footer hints.

## WiFi / clock
- `net_wifi_start()` loads NVS WiFi creds first, then starts WiFi STA + SNTP.
- Last epoch saved under NVS `net/last_t`; on boot, table clock can show last-known timestamp with `OLD` marker.
- On SNTP lock: time synced, ding, then WiFi stops after sync; otherwise off after timeout.
- Periodic epoch save while online.

## Important files
- `main/car.c` — control loop, safety, boot/self-test orchestration, pin definitions.
- `main/car_os.c/h` — Car OS context, states, screen dispatch.
- `main/os_screens_tft.c` — standby clock, settings, hub, score, trip, conn, companion.
- `main/led_matrix.c/h` — WS2812 23x13 driver, mapper, brightness, limiter.
- `main/matrix_layout_23x13_299.h` — custom 23x13 LED map.
- `main/matrix_fx.c` / `matrix_fx.h` — particle engine for matrix shows (fireworks); shared `hsv_rgb()` / `hsv_put()` colour helpers.
- `main/game_serpent.c/h` — active arcade snake.
- `main/serpent_levels.h` — 20 level layouts.
- `main/net_wifi.c/h` — WiFi/SNTP/last-known epoch.
- `main/imu_adv.c/h`, `imu_driver.c/h` — disabled if MPU absent.

## Build / flash / monitor
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_TOOLS_PATH='C:\idf-tools'
. 'C:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1'
cd 'C:\Users\Azam Khan\Videos\xbox360_controller'
idf.py build
idf.py -p COM4 -b 460800 app-flash
idf.py -p COM4 monitor
```

## Expected boot logs
- `pin check: 29 functions, no GPIO conflicts`
- `matrix init OK`
- `NEON SERPENT V2 init ...`
- `selftest done: 15 PASS 0 FAIL`
- `net_wifi: last known epoch=...`
- `net_wifi: connected, IP assigned`
- `net_wifi: SNTP synced, epoch=...`
- `net_wifi: WiFi OFF (time synced)`

## NVS keys
- `car_set`: speed/volume/obstacle/led/tft brightness/mist/gears/roof/mx_rgb/mx_ori/mx_idle/mx_fx
- `net`: wifi_ssid, wifi_pass, last_t
- `arcade`: serp_hi3, serp_best3, serp_long3, serp_combo3, serp_games3, serp_foods3, serp_clear3, rumble
- Inactive migration keys: serp_hi2, serp_lvl
