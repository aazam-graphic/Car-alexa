/*
 * xbox_bt_bridge.h - Xbox pad -> BLE HID gamepad bridge (mobile gaming).
 *
 * The USB dongle in this build only works on the ESP32-S3 (USB host). Phones
 * have no USB host for that dongle, so this module re-publishes the Xbox pad
 * as a Bluetooth LE HID gamepad.
 *
 * Two identities, switchable at runtime and persisted in NVS:
 *   XBOX    - impersonates an Xbox Series X|S wireless pad (045E:0B13)
 *   GENERIC - plain BLE gamepad (fallback if a game rejects the Xbox one)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XBOX_BT_PROFILE_GENERIC = 0,
    XBOX_BT_PROFILE_XBOX    = 1,
} xbox_bt_profile_t;

/* Bring up the BLE stack + HID GATT service (safe to call once at boot). */
void xbox_bt_init(void);

/* Enable/disable the bridge. Off = stop advertising + drop connection. */
void xbox_bt_set_enabled(bool on);
bool xbox_bt_is_enabled(void);

/* True when a phone/PC is paired and subscribed to the input report. */
bool xbox_bt_connected(void);

/* Active identity. Changes drop the link and restart advertising - the phone
   has to forget the old bond and pair again to pick up the new name. */
xbox_bt_profile_t xbox_bt_profile(void);
void xbox_bt_set_profile(xbox_bt_profile_t p);

/* Short label for the UI: "XBOX 0B13" / "GENERIC". */
const char *xbox_bt_profile_name(void);

/* Push the latest pad state to the connected host (call ~50 Hz). */
void xbox_bt_poll(void);

#ifdef __cplusplus
}
#endif
