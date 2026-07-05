#pragma once
#include <stdint.h>

class TFT_eSPI;

// K10 ILI9341 video output for the nofrendo NES core.
namespace k10video {

// Initialise the TFT (landscape 320x240), turn on the backlight, and build
// the NES -> RGB565 palette. Call once after k10.begin().
void init();

// Blit one NES frame to the LCD. `bmp` is nofrendo's indexed framebuffer:
// 272 bytes/row (8px overdraw each side), 256 visible columns, 240 rows.
// Installed as nofrendo's per-frame blit callback.
void blit(const uint8_t *bmp);

// Borrow the shared TFT (e.g. to draw the game-selection menu before the NES
// frame loop takes it over). Available after init().
TFT_eSPI& display();

} // namespace k10video
