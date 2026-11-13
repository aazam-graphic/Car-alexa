/*
 * xbox_bt_bridge.h - Xbox pad -> BLE HID gamepad bridge (mobile gaming).
 *
 * The USB dongle in this build only works on the ESP32-S3 (USB host). Phones
 * have no USB host for that dongle, so this module re-publishes the Xbox pad
 * as a standard Bluetooth LE HID gamepad. Pair the phone with "Azam Xbox Pad"
 * and any mobile game sees a normal gamepad.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the BLE stack + HID GATT service (safe to call once at boot). */
void xbox_bt_init(void);

/* Enable/disable the bridge. Off = stop advertising + drop connection. */
void xbox_bt_set_enabled(bool on);
bool xbox_bt_is_enabled(void);

/* True when a phone/PC is paired and subscribed to the input report. */
bool xbox_bt_connected(void);

/* Push the latest pad state to the connected host (call ~50 Hz). */
void xbox_bt_poll(void);

#ifdef __cplusplus
}
#endif
