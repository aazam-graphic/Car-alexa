# AZAM CAR OS + 120-LED MATRIX ARCADE — Full Project Details

> **Last updated: 5 Oct 2026 — source code se verified** (`main/*.c/h`, build log, hardware play-test).
> Alexa Oct 2026 me fully remove ho gayi (bridge/task/UI/deps). WiFi/SNTP raha (table clock).

**Project:** Xbox 360 wireless (Redgear Pro dongle) se chalne wala Robot Car + "Car OS" Dashboard + 120-LED RGB Matrix Arcade
**MCU:** ESP32-S3 N16R8 (16 MB Flash, 8 MB Octal PSRAM, dual-core 240 MHz) | **Framework:** ESP-IDF v5.5.5 (build-tested) / v6.0.2 (original)
**Project name:** `xbox360_controller`

**Ek line mein:** Gamepad → ESP32-S3 (USB Host) → Motors (UART slave) + Lights + Servos + Sensors + TFT/OLED UI + WiFi/Alexa + 120-LED Matrix (games/show).

---

## 1. Boot Flow + FreeRTOS Tasks

```
app_main() → 4 tasks:
  1. usb_host  (Core 0, prio 2, 4 KB)   - USB Host library events
  2. xbox360   (Core 0, prio 3, 8 KB)   - gamepad driver (XBOXRECV port)
  3. car       (Core 1, prio 2, 16 KB)  - control + Car OS + HUD + matrix + arcade
  4. alexa     (Core 1, prio 1, 16 KB)  - WiFi + MQTT/TLS cloud bridge
+ late_init_task (background): mic → sounds → images → snd preload → boot chime
+ imu_adv fast task (100 Hz MPU pipeline, PART 4)
+ mic capture task (I2S1, PART 8)
```

**Boot sequence:** `BOOT splash → STANDBY (table clock, motors locked) → START se HOME → DRIVE (arming 1.3 s + neutral gate)`.

---

## 2. Hardware Components (sab)

| # | Component | Detail | Connection |
|---|-----------|--------|------------|
| 1 | ESP32-S3 DevKit N16R8 | 16 MB Flash, 8 MB Octal PSRAM | Main board |
| 2 | Xbox 360 Wireless Receiver (Redgear Pro) | USB Host 045E:028E/0719, max 4 pads | Native USB-OTG (GPIO 19/20) |
| 3 | 2.8" SPI TFT ILI9341/ST7789 | 320×240 landscape RGB565, 40 MHz SPI | SPI + GPIO |
| 4 | 0.96" OLED SSD1306 | 128×64, I2C 0x3C, mini HUD/OS mirror | I2C |
| 5 | MPU advanced pipeline | Pitch/roll/yaw, tilt/rollover/freefall/drift guards | I2C bus |
| 6 | 3× HC-SR04 Ultrasonic | Left/Front/Right cm (rear REMOVED) | GPIO trig/echo |
| 7 | MAX98357A I2S Amp + Speaker | Engine, horn, SFX, clicks, TTS PCM | I2S |
| 8 | D1 Motor Driver Slave (UART) | `L<pwm>,R<pwm>` @115200, real brake | UART TX |
| 9 | 2× Servo Pan/Tilt | Spark cannon aim, LEDC ch0/ch1 | GPIO 4/5 |
| 10 | WS2812 Rear Strip | 2 px (→6 LEDs) + 12 under-car daisy-chain | GPIO 6 |
| 11 | 2× Front WS2812 | Left + Right indicator/headlight glow | GPIO 17/15 |
| 12 | Relays active-LOW | Headlight 46, Rear/spoiler 3, Mist 16 | GPIO |
| 13 | **120-LED Matrix 12×10** | WS2812B serpentine, DIN GPIO 18, cap 72/255 | GPIO 18 (room/roof LED NIKALI — takeover) |
| 14 | MEMS Mic INMP441-class | I2S1 16 kHz, beat/clap/VU, 64 KB PSRAM ring | SCK13/WS14/SD48 |
| 15 | WiFi + AWS IoT Alexa | MQTT/TLS 8883, Device Shadow, SNTP IST clock | On-chip radio |

**Removed:** rear ultrasonic, gripper/spoiler servos, battery ADC (GPIO1=UART TX), buzzer (I2S horn), body-backlight relay (GPIO14=mic WS), roof WS2812 (GPIO18=matrix).

---

## 3. Pin Map (Oct 2026, `main/car.c` + module headers se)

| GPIO | Function | Notes |
|------|----------|-------|
| 0 | TFT DC | strapping pin — pull-up rakho |
| 1 | UART TX → D1 motors | 115200 |
| 2 | TFT RST | |
| 3 | Rear light relay (active LOW) | strapping — JTAG source |
| 4 / 5 | Servo PAN / TILT | LEDC ch0/ch1 |
| 6 | Rear (2 px) + Under (12 px) WS2812 | single daisy-chain |
| 7 | US FRONT Trig | |
| 8 / 9 | US RIGHT Trig / US LEFT Echo | |
| 10 / 11 | US FRONT Echo / US RIGHT Echo | |
| 12 | US LEFT Trig | |
| 13 / 14 | MIC SCK / MIC WS | I2S1 |
| 15 / 17 | Front Right / Front Left WS2812 | |
| 16 | Mist relay (active LOW) | |
| 18 | **MATRIX 120 DIN** | roof LED yielded; RMT non-DMA 48 symbols |
| 19 / 20 | USB-OTG (dongle) | haath mat lagao |
| 21 | TFT SCLK | 40 MHz SPI |
| 38 | TFT MOSI | MISO unconnected |
| 39 / 47 / 40 | I2S BCLK / LRC / DIN | MAX98357A |
| 41 / 42 | I2C SCL / SDA | OLED + IMU |
| 43 / 44 | UART0 console | flash/monitor |
| 45 | TFT CS | strapping — VDD_SPI |
| 46 | Headlight relay (active LOW) | strapping — boot mode |
| 48 | MIC SD | I2S1 data |
| 26–32 / 33–37 | Flash / Octal PSRAM | kabhi use mat karo |

Boot guard: `pin_conflict_check()` (`main/car.c:167`) — `pin check: 29 functions, no GPIO conflicts`.

---

## 4. Button Maps

### 4.0 Priority (hamesha)
1. **GUIDE tap = E-STOP toggle** (stationary hona chahiye clear ke liye) — har context me.
2. Disconnect = fail-safe (motors 0, arcade pause+dim).
3. Arcade active ho to input arcade ka (car controls frozen, motors forced 0).
4. `os_parked()` true ho to motors 0 (menus/TFT games).

### 4.1 DRIVE Mode (manual)
| Button | Kaam |
|--------|------|
| Left stick | Throttle + steering (deadzone ±1500) |
| RT | Proportional speed cap (trigger zones) |
| LT | Progressive brake, 200+ = real brake + brake light |
| A tap / hold 700 ms | Headlight / mist toggle; double-tap = pass-flash |
| B tap / double-tap / hold | Roof toggle (legacy) / roof mode cycle / roof panel |
| X tap / hold 1 s | AUTO preview / AUTO enter; double-tap = mic VU overlay |
| Y tap / hold / double-tap | Ambient preset / color picker / under-strip solo |
| LB hold / tap | Horn / GEAR DOWN (paddle) |
| RB hold / tap | TURBO (3 s + 5 s cooldown) / GEAR UP |
| LS / RS click | CRAWL toggle / cannon recenter |
| D-pad | Hazard / indicators / rear-light / gripper sound |
| START tap / hold | Home / Quick Settings (QCC) |
| BACK tap / hold / 1.5 s | Cockpit view cycle / theme cycle / Motion Radar |
| GUIDE | E-STOP |

### 4.2 Car OS Menus (HOME/SETTINGS/DIAG/OLED/GAMES)
D-pad/stick navigate (repeat 350 ms/120 ms), **A** confirm/apply, **B** back/cancel, **START** save/pause, **BACK** back, **GUIDE** estop. DIAG Restart/Factory Reset = **A hold 1.5 s**.

### 4.3 TFT Games (NEON CONVOY `cargame_neon_convoy.c`, NEON SERPENT `cargame_neon_serpent.c`)
Stick+buttons game actions, START pause (release resume), BACK/B exit hub, motors parked.

### 4.4 Matrix Arcade — NEON SERPENT 10-LEVEL (`main/game_serpent.c`, `serpent_levels.h`)
**Entry:** HOME **SNAKE tile + A**, ya Games Hub **MATRIX SNAKE tile + A**, ya parked me **BACK hold 1 s** → MENU (TFT toast + companion screen).
| Button | Kaam |
|--------|------|
| X / Y / B / A | Left / Up / Right / Down — sirf PLAY me (D-pad + left stick bhi chalte hain) |
| START | Menu start / pause-resume / game-over retry |
| BACK tap / hold | Menu wapas / arcade se bahar (exit flash) |
| LB / RB (MENU) | Rumble mode − / + (OFF/SOFT/FULL) |
| D-pad / stick (MENU) | Start level select (unlocked tak) |
| GUIDE tap / hold | Bahar + E-STOP path / bahar bina E-STOP |
| Disconnect | Pause + 10% dim + red border pulse |

Rules: **10 levels** (speed 320→120 ms, wrap L1-6, wall L7-10 per table B+, endless `max(100, 120-20×food/5)`), **3 lives** (DYING → RETRY digits+lives dots → INTRO), scoring food 10×L / bonus 50×L fast, 20×L slow / clear 50×L (+500×L board-full), steady **red food**, static **purple mines** + patrol (L8 row 2, L10 row 1), **gold bonus 2×2** (7 s), green snake + lime head, direction queue-2 + anti-reverse + arming rule, accumulator timing, TFT companion (mirror + score/level/food/lives/rumble), hiscore `serp_hi2` (level-1 runs only) + practice mode.

### 4.5 Input Event Engine (`main/input_events.c/h`)
TAP 350 ms / HOLD 700-2000 ms / REPEAT 350+120 ms / DOUBLE-TAP / CHORD — tap-hold exclusive, consumable, context-switch flush (`input_flush_context_switch`, GUIDE excluded).

---

## 5. Car OS Screens (`main/car_os.c/h`, `main/os_screens_tft.c`)

| Screen (enum) | Display |
|---|---|
| BOOT | AZAM CAR OS logo anim + checklist CONTROL/SENSORS/DISPLAY/PAD |
| **STANDBY (TABLE CLOCK)** | Cream card, giant 7-seg HH:MM:SS + AM/PM, date top, weekday + numeric date, seconds line, `PRESS START`; sync se pehle `--:--`; SNTP lock par `TIME SYNCED - IST` toast 1.5 s (`net_wifi.c` IST-5:30) |
| HOME (9 + 5 tiles, RB page) | P0: DRIVE SNAKE SETTINGS GAMES DIAG RADAR ROOF TRIP NOTIFY; P1: OLED GRAPHS SCORE LINK CLOCK (VLOG/empties removed Oct 2026) |
| DRIVE (cockpit 4 views, 10 FPS redraw) | Speed hero digits, L/F/R cards (cyan/amber/red), gear pills G1–G5, 6 status icons; BACK tap = view (FULL/COMPACT/SENSOR/NIGHT), BACK hold = theme (NIGHT/SOLAR/SUN) (`ui_drive_cockpit.c`) |
| ANALYTICS (10 FPS) | 480-sample PSRAM graphs speed/front/yaw |
| OLED CTRL | Layout MINI HUD/RADAR/STATUS/TEXT + 2× preview |
| SETTINGS (19) | SPEED CAP, ENGINE VOL, OBST DIST, LED BRIGHT, GEAR 1–5, OLED LAYOUT, HUD LAYOUT, MIST MAX, TFT BRIGHT, RUMBLE (OFF/SOFT/FULL), RGB BRIGHT (10-72 live), MATRIX ORIENT (LANDSCAPE/PORTRAIT live), IDLE SHOW (FALAK/SWEEP/OFF live), GAME FX (FULL/MINIMAL), SAVE — A apply, START NVS save, B discard |
| GAMES HUB | 3 games: 2 TFT + MATRIX SNAKE tile (matrix launch, exit → hub) |
| MATRIX companion (arcade active) | Live mirror + score/level/food/lives/rumble/mode/bonus + hints; full-push only (partial corrupts panel), PLAY 10 FPS |
| DIAG (5 tabs, 2 FPS) | SYS (heap/PSRAM/uptime) / SENS / CTRL / DISPLAY (FPS/push/dirty) / THEME + Restart/Factory Reset (A 1.5 s) |
| NOTIF / SCORE / TRIP / CONN | 24-entry feed (Safety/Rules/Sound/MPU), drive grade, odometer (trip-based), WiFi/time status |
| Quick overlay (START hold) | HEAD/HAZ/ROOF/MUTE chips + BRIGHT/VOLUME/THEME sliders (PROFILE removed with Alexa) |
| DRIVE_ARMING | 1.3 s anim + sticks neutral 250 ms gate |

---

## 6. Modules + Functions (public API)

| File | Kaam + key functions |
|---|---|
| `main.c` | `app_main()` — 3 tasks create (usb_host, xbox360, car). Alexa task Oct 2026 me removed |
| `xbox360.c/h` | USB Host driver; `xbox360_pad(slot)`, `xbox360_rumble(slot,l,r)`, `xbox360_dongle_connected()`, LED ring, alive-timestamp |
| `car.c/h` | Control loop `car_task()` (arcade me fixed 20 ms `vTaskDelayUntil`, P5 loop stats); `input_handle`, `manual_logic`, `auto_logic` (CRUISE→SEARCH), `drive_output`, `strip_update`, `audio_render/effect_update`, `hw_init`, `pin_conflict_check`; settings `car_get/set_setting`, gear caps, `car_settings_save/factory_reset`, `car_system_restart`, display/matrix settings get/set + `car_display_apply_saved`; trip `trip_*`; SFX `car_sfx_*`, `car_snd_play_pcm`, TTS; OLED wrappers `osd_*`; `car_roof_apply_saved`, `car_drive_locked`, `car_get_yaw`, `car_us_front_ok` |
| `car_os.c/h` | `os_init/handle_input/update/draw_tft/draw_oled`, `os_context/parked/game_active`, `os_alert`, `os_request_screen`, `os_win_push/pop`, `os_hist_*`, `os_perf`, `car_os_ctx`, state names/hints; TFT screens 10/2 FPS throttled (lag fix Oct 2026) |
| `os_screens_tft.c` + `os_gfx/os_widgets/os_assets/ns_theme` | Sab screen renderers + standby clock + companion + dirty-skip (full pushes; partial corrupts panel) |
| `tft_display.c/h` + `tft_boot_anim.c` | SPI DMA driver, framebuffer push, brightness, `tft_set_flip` (baseline assert), boot anim |
| `os_oled.c` + `oled_driver.h` | OLED HUD 4 layouts (arcade PLAY states me suppressed) |
| `roof_light.c/h` | WS2812 roof (GPIO18) — **YIELDED** (matrix takeover); 8 modes, NVS persist |
| **`led_matrix.c/h`** | 120-LED driver 12×10 + 10×12 portrait transpose (`matrix_xy_to_index[_portrait]`, `matrix_field_w/h`); `matrix_set_orient` (LANDSCAPE/PORTRAIT), `matrix_set_idle` (FALAK/SWEEP/OFF), owner acquire/release, 1500 mA limiter, pause dim, exit flash, boot self-test 3 s |
| **`game_serpent.c/h` + `serpent_levels.h`** | 10-level arcade: `route` (input) + `tick` (logic/render), queue-2, accumulator, BFS spawns, patrol, 3 lives, RETRY/CLEAR/WIN_BOARD, rumble queue + OFF/SOFT/FULL, `serp_hi2`/practice/`serp_lvl`, status snapshot + mirror, hub stubs, `serpent_arcade_open/hiscore` |
| **`falak_show.c/h`** | Idle show `falak_tick` (field-size aware heart + marquee) |
| `input_events.c/h` | semantic events + `input_flush_context_switch` (arcade entry/exit par bhi) |
| `imu_adv.c/h` + `imu_driver.c` | 100 Hz pipeline: `imu_adv_init/ready/slow_update/set_motion`, `heading_rel`, `evt_recv`, `snapshot`, `motors_cut`, `rollover_latched/ack`, `corner_cap`, `airborne`, `drive_score`, `trip_hard_bumps` |
| `ui_drive_cockpit.c/h` + `ui_motion_radar.c` | `ui_cockpit_draw/next_view/next_theme`, battery %, dim-rect; radar UI |
| `mic_in.c/h` | `mic_init/ready/poll`, `mic_rms/vu/peak/beat/clap`, 3 s `mic_listen_*` (SCK13/WS14/SD48) |
| `snd_bank.c/h` + `img_bank.c/h` | SPIFFS→PSRAM LRU: `snd_play[/_vol]`, `snd_bank_init/prefetch/preload_all`; `img_get/init`, `img_blit[/_half/_full]` |
| `notif.c/h` | `notif_init/push/list`, 24-entry ring 5 categories |
| `mem_diag.c/h` | `mem_diag_init/start_timer/tick` (heap/PSRAM/stack, 10 s) |
| `net_wifi.c/h` | `net_wifi_start/get_state/up/time_synced` (station + SNTP IST; Alexa gayi, clock raha) |
| `car_games.c/h` + `cargame_*` | TFT games registry (3: Convoy, Serpent-g7, Matrix stub) + hub |

---

## 7. Drive Modes + Safety
MANUAL / CRAWL (LS) / AUTO (X hold, states CRUISE SLOW TURN REVERSE ESCAPE STUCK PAUSE SEARCH, stuck-escape + 360 search). Gears G1–G5 default 5/15/25/35/50 (NVS, turbo bypass). Safety: E-STOP sticky, disconnect fail-safe, 250 ms neutral-release lock, IMU tilt/rollover/freefall cut + corner cap + drift slow, alert priority ESTOP>PAD>SENSOR>OBSTACLE, roof safe-off, arcade motor park.

## 8. Memory / Partitions (16 MB)
`nvs 24K | otadata 8K | phy 4K | factory 6M (app ~1.46 MB, ~77% free) | storage SPIFFS ~9.9M`. RAM: internal ~128 KB free heap, PSRAM ~8 MB free (analytics 480×3, asset cache, mic ring). Build: `xbox360_controller.bin` ≈ 0x167f20.

## 9. Build & Flash (is PC par working)
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_TOOLS_PATH='C:\idf-tools'   # junction → ~/.espressif (username me space hai, GCC toot-ta hai)
. 'C:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1'
cd 'C:\Users\Azam Khan\Videos\xbox360_controller'
idf.py build
idf.py -p COM4 -b 115200 flash   # 460800 kabhi packet-error de to 115200
idf.py -p COM4 monitor
```
Verify: `pin check: 29 functions, no GPIO conflicts`, `matrix init OK: 12x10=120`, `serpent arcade init`, no panic.

## 10. NVS Keys
`car_set`: spd/vol/obs/led/tftb/mist/gears/roof_m/roof_b/roof_c/mx_rgb/mx_ori/mx_idle/mx_fx (+VER=5) · `net`: wifi_ssid/wifi_pass · `arcade`: serp_hi2/serp_lvl/rumble.
⚠ WiFi creds source me embedded — public repo se pehle rotate karo.

## 11. P5 Numbers (measured, Oct 2026)
Arcade loop: min 15 / avg 20 / max 25 ms (fixed 20 ms pacing, FreeRTOS 100 Hz tick granularity) · OLED calls in PLAY = 0 · Steps L1 315/325 ms (target 320) · Dead files 12 + alexa_bridge/config (68 KB src) + mqtt/cjson deps removed · App flash ~1.5 MB @460800.
