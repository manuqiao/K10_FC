#pragma once
#include <stdint.h>

// Bluetooth-controller input source + scan/pair UI for the K10 NES player.
//
// Wraps libs/BLE_FFF0 (a screen-agnostic BLE Central for HM-10-style transparent-
// UART peripherals, service 0xFFF0 / char 0xFFF1). After begin() + a successful
// connect_flow(), read() returns a NES-pad bitmask (same bit layout as k10input)
// assembled from the controller's per-key event bytes:
//
//   byte = (pressed ? 0x80 : 0) | code,   code 1..8 =
//     1=Up 2=Down 3=Left 4=Right 5=A 6=B 7=Select 8=Start
//
// The scan/pick/connect screens are drawn with TFT_eSPI (k10video::display()) and
// operated by the BOARD buttons (k10input), since the controller isn't connected
// during pairing:  A = next device, B = connect, hold A+B = rescan.
namespace k10ble {

// Bring up BLE. Call once (idempotent). Wires the data/disconnect/log callbacks
// and starts the FFF0 disconnect-monitor task + auto-reconnect (3 tries).
void begin();

// NES-pad bitmask from the connected controller (instant; updated by the BLE
// notify callback). 0 when nothing is held or the link is down.
uint8_t read();

// True while the FFF0 link is up.
bool isConnected();

// Scan -> pick -> connect UI loop. Blocks until a device is connected; re-scans
// on an empty scan or a failed connect. Driven by the board buttons. Returns
// true once paired. Call only after begin() + k10input::init() + k10video::init().
bool connect_flow();

} // namespace k10ble
