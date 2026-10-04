# NEON SERPENT V2

Target: existing ESP32-S3 Car OS, 120 RGB LEDs, Xbox 360 receiver and existing TFT dashboard.

## Controls
- Y = UP
- A = DOWN
- X = LEFT
- B = RIGHT
- D-pad = ignored during gameplay
- Left/right sticks = ignored
- LT/RT = ignored
- LB/RB = ignored during gameplay (menu only: rumble mode)
- START = start / pause / resume / retry
- BACK tap = menu
- BACK hold = exit

## Gameplay upgrades
- 20 handcrafted levels
- Endless mode after level 20
- Fixed-timestep simulation
- Smart reachable-food spawning
- Normal / Golden / Prism food
- Shield / Slow / X2 Score / Ghost power-ups
- Combo multiplier up to x5, plus X2 power
- Risk and speed bonuses
- Time / lives / combo / objective level-clear bonuses
- Static mines + patrol + predictive hunter hazards
- 16-particle fixed pool
- Layered RGB animation and level palettes
- Xbox haptic event patterns
- Existing sound-bank event hooks
- NVS career statistics and high score
- TFT score / level / food / lives / power / combo / elapsed-time display

## Changed files
- `main/game_serpent.c`
- `main/game_serpent.h`
- `main/serpent_levels.h`
- `main/os_screens_tft.c`

## Validation
Host-side C syntax check was run on `main/game_serpent.c` with minimal ESP/NVS stubs and completed successfully.
A real ESP-IDF firmware build/flash was not available in this environment, so hardware validation is still required.
