#pragma once
#include <stdint.h>

// External 4x2 matrix keypad for the K10 NES player.
//
// Wiring (pull-up-FREE topology — no external resistors, nothing to solder):
//   Rows (outputs, driven LOW one at a time, others HIGH):  P2, P3, P8, P13
//   Cols (inputs, native ESP32 INPUT_PULLUP):               P0, P1
//
// Why these pins: the K10 exposes only TWO native ESP32 edge pins — P0 (GPIO1)
// and P1 (GPIO2). They are the ONLY pins whose internal pull-up Arduino's
// INPUT_PULLUP can enable. Every other edge pin (P2, P3, P8, P13, ...) is on
// the on-board I2C GPIO EXPANDER, whose digital_read exposes no pull config and
// floats as an input (confirmed: random "jumping" presses). So the matrix's two
// INPUT lines (the columns) MUST be P0/P1; the four OUTPUT lines (the rows) go
// on expander pins, where floating is irrelevant because they actively drive.
//
// Pin facts (framework pins_arduino.h + initBoard.h ePin_t):
//   - P0 (GPIO1), P1 (GPIO2): native -> Arduino pinMode(INPUT_PULLUP)/digitalRead.
//   - P2/P3/P8/P13: expander -> digital_write(ePn) drives them as outputs. Each
//     expander transaction is a bus write (faster than the ~11ms read debounce),
//     and the scan runs on a background core-0 task, never the audio frame loop.
//
// Keymap — entries in k10_matrix.cpp are permuted to match this keypad's
// physical wiring, so each physical button reports its silkscreened NES
// function. In row-major scan order (r0c0, r0c1, r1c0, ...) this yields:
// left, select, down, start, up, b, right, a. If your physical keypad's button
// order differs, permute kKeymap in k10_matrix.cpp — that table is the only
// thing to touch.
//
//              col0 (P0)     col1 (P1)
//   row0 P2      Left          Select
//   row1 P3      Down          Start
//   row2 P8      Up            B
//   row3 P13     Right         A
namespace k10matrix {

// Configure pins and start the background scan task (idempotent). Must be
// called AFTER k10.begin() (which brings up the I2C bus + expander).
void init();

// Freeze the background scan task — used when another input source takes over
// (e.g. the ADKeyboard) so the matrix's expander writes stop contending for the
// I2C bus. Idempotent. read() returns the last cached value after this.
void suspend();

// Current debounced key bitmask as a NES-pad byte (bits match nofrendo's
// NES_PAD_* / k10input's NP_*), so the menu and NES core consume it directly:
//   bit0 A   bit1 B     bit2 Select bit3 Start
//   bit4 Up  bit5 Down  bit6 Left   bit7 Right
uint8_t read();

} // namespace k10matrix
