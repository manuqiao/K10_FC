#pragma once
#include <stdint.h>

// External ADKeyboard (DFR0075, 5-key analog keypad) plugged into the K10 IO
// Extender's C0 port, as a NES-pad input source.
//
// IMPORTANT — this is NOT a K10 GPIO/ADC. The keypad sits on the IO Extender
// board (SKU DFR1231); the extender's own chip (I2C address 0x33, on the same
// Wire bus as the on-board expander / accel / light sensor) measures C0's
// voltage and exposes a 12-bit ADC over I2C. So reading a key is an I2C
// transaction to 0x33, not analogRead(). Protocol was lifted from DFRobot's
// extender library: set C0 mode to ADC (reg 0x2c), then read 3 bytes from the
// ADC register (reg 0x45) -> [status, hi, lo]; status 0x01 = data ready.
//
// The ADKeyboard only has 5 keys, so it covers the D-pad (s2/s3/s4/s5) + NES A
// (s1). SELECT/START are borrowed from the board's own two buttons:
//   board A -> SELECT     board B -> START
//
// Keymap (per spec — permute kKey2NES in k10_adkey.cpp if your wiring differs):
//   s1 -> NES A      s2 -> Up      s3 -> Left      s4 -> Down      s5 -> Right
namespace k10adkey {

// Tell the extender to put C0 in ADC mode and start the background poll task.
// Must be called AFTER k10.begin() (brings up the shared I2C bus). Idempotent.
void init();

// Returns a NES-pad bitmask (NP_* layout) sampled by the background task:
//   bit0 A   bit1 B     bit2 Select bit3 Start
//   bit4 Up  bit5 Down  bit6 Left   bit7 Right
uint8_t read();

} // namespace k10adkey
