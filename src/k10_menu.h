#pragma once
#include <stddef.h>
#include <stdint.h>
#include "rom_catalog.h"   // RomEntry

// Full-screen menus drawn on the K10 LCD.
//
// Boot control-mode chooser + the game-selection list. Both are driven by a
// pluggable NES-pad input source so the SAME game list serves the board's local
// controls (k10input) and a paired Bluetooth controller (k10ble).
namespace k10menu {

// A NES-pad bitmask source: returns the instantaneous button bits (layout matches
// nofrendo's NES_PAD_* / k10input's NP_*).
using InputFn = uint8_t (*)();

enum ControlMode { MODE_LOCAL, MODE_BLUETOOTH, MODE_BT_HID, MODE_MATRIX };

// Boot screen: pick how the player will control the NES. Board A toggles the
// highlighted row, board b confirms. Blocks until a choice is made.
ControlMode select_mode();

// Game-selection list.
//   roms/count : the embedded ROM catalogue.
//   read       : NES-pad source used to navigate (local k10input or BLE k10ble).
//   dpadNav    : true for a source that exposes a real D-pad (BLE) -> Up/Down
//                move and A/Start confirm; false for the board (tilt would
//                otherwise bleed into navigation) -> board A moves, board B
//                confirms, exactly as before.
// Blocks until a game is chosen; returns its index, or -1 if count==0.
int select_game(const RomEntry *roms, size_t count, InputFn read, bool dpadNav);

} // namespace k10menu
