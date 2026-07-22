#pragma once
#include <stdint.h>

// Bluetooth **HID gamepad** NES input source for the K10 NES player.
//
// Targets STANDARD BLE HID gamepads via libs/BLE_HID_Gamepad (which in turn owns
// a libs/BLE_HID_Host, service 0x1812). The lib handles everything up to the
// decoded {buttons, hat} report — bonding (just-works), subscribing, and the
// scan/pick/connect TFT menu — and is NES-agnostic. THIS file keeps only the
// NES button mapping: onReport() turns each report into a NES-pad bitmask.
//
// After begin() + a successful connect_flow(), read() returns a NES-pad bitmask
// (same bit layout as k10input):
//   D-pad from the report's hat (Usage 0x39), A/B/Start/Select from the button
//   field (Usage Page 0x09) via the BUTTON2NES table in the .cpp.
namespace k10blehid {

// Bring up the BLE HID host. Call once (idempotent). Configures just-works
// bonding, wires the report/disconnect/log callbacks, and starts the drop +
// auto-reconnect (3 tries) monitor.
void begin();

// NES-pad bitmask from the connected gamepad (instant; updated by the notify
// callback). 0 when nothing is held or the link is down.
uint8_t read();

// True while the HID link is up.
bool isConnected();

// Scan -> pick -> connect UI loop. Blocks until a device is bonded + subscribed;
// re-scans on an empty scan or a failed connect. Driven by the board buttons.
// Returns true once paired. Call only after begin() + k10input::init() +
// k10video::init().
bool connect_flow();

} // namespace k10blehid
