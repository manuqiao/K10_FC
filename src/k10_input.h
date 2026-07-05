#pragma once
#include <stdint.h>

// K10 input -> NES gamepad mapping.
//
// The K10 has only two physical buttons (A, B), an accelerometer, and an
// ambient-light sensor, so:
//   - Tilt the board   -> D-pad (left/right/up/down)
//   - Board B          -> NES A (jump)
//   - Board A          -> NES B (shoot)
//   - Cover light sens -> START (begin game / pause)  [ambient ALS reading < 20]
//   - Hold A+B ~0.6s   -> SELECT
//
// Tunables live at the top of k10_input.cpp so the axis mapping / deadzone
// can be adjusted without touching anything else.
namespace k10input {

void init();

// Returns a NES pad bitmask (bits match nofrendo's NES_PAD_* in nes/input.h).
uint8_t read();

} // namespace k10input
