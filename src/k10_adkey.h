#pragma once
#include <stdint.h>

// External ADKeyboard (DFR0075, 5-key analog keypad) plugged into the K10 board's
// OWN Gravity IO interface — the 3-pin PH2.0 full-function analog port — as a
// NES-pad input source.
//
// That port is a NATIVE ESP32-S3 ADC1 pin (A0 = GPIO1 = ADC1_CH0; the second
// Gravity port is A1 = GPIO2 = ADC1_CH1). ADC1 is WiFi-safe (ADC2 is not). So
// reading a key is just analogRead() on ADKEY_PIN (set in k10_adkey.cpp, default
// A0) — NOT an I2C transaction. (An earlier revision of this mode wired the
// keypad to the IO Extender board's C0 and read it through the extender chip at
// I2C 0x33; that path lives in git history if you ever switch back to the
// expansion board — the calibration thresholds would then need re-measuring too.)
//
// The ADKeyboard only has 5 keys, so it covers the D-pad (s2/s3/s4/s5) + NES A
// (s1). SELECT/START are borrowed from the board's own two buttons:
//   board A -> SELECT     board B -> START
//
// Keymap (per spec — permute kKey2NES in k10_adkey.cpp if your wiring differs):
//   s1 -> NES A      s2 -> Up      s3 -> Left      s4 -> Down      s5 -> Right
namespace k10adkey {

// Start the background poll task that samples the ADKeyboard via analogRead().
// Must be called AFTER k10.begin(). Idempotent. main() also suspends the board
// input poller and matrix scan first — k10adkey reads board A/B on the I2C
// expander itself for SELECT/START, and Wire isn't thread-safe between tasks.
void init();

// Returns a NES-pad bitmask (NP_* layout) sampled by the background task:
//   bit0 A   bit1 B     bit2 Select bit3 Start
//   bit4 Up  bit5 Down  bit6 Left   bit7 Right
uint8_t read();

} // namespace k10adkey
