#include "k10_video.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"
#include "initBoard.h"   // eLCD_BLK, digital_write()

extern "C" {
#include "nofrendo.h"    // nofrendo_buildpalette, NES_PALETTE_*
#include "nes/nes.h"     // NES_SCREEN_* geometry
}

// K10 panel is 320x240 in landscape; NES is 256x240 -> centred with side bars.
#define DISP_W   320
#define DISP_H   240
#define NES_W    256                       // NES_SCREEN_WIDTH
#define NES_H    240                        // NES_SCREEN_HEIGHT
#define NES_PITCH NES_SCREEN_PITCH          // 272 (8px overdraw each side)
#define OFFX     ((DISP_W - NES_W) / 2)     // 32 px left/right bars

static TFT_eSPI tft = TFT_eSPI();
static uint16_t *g_palette = nullptr;   // 256 RGB565
static uint16_t *g_rgb = nullptr;       // NES_W*NES_H RGB565 scratch (PSRAM)

namespace k10video {

void init()
{
    digital_write(eLCD_BLK, 1);   // backlight on (via K10 GPIO expander)

    tft.begin();
    // Landscape, rotated 180° (was 1). The player holds the board flipped so
    // the buttons are on the preferred end; rotation 3 keeps the picture
    // right-side-up in that grip. Holding it flipped also inverts the
    // accelerometer's X/Y, so the tilt signs in k10_input.cpp are flipped to
    // match. To go back to the old orientation, set this to 1 AND flip both
    // LR_SIGN / UD_SIGN in k10_input.cpp back to +1.
    tft.setRotation(3);
    tft.fillScreen(TFT_BLACK);

    g_palette = (uint16_t *)nofrendo_buildpalette(NES_PALETTE_PVM, 16);

    // 256*240*2 = ~120 KiB -> keep it in PSRAM so internal RAM stays free.
    // If PSRAM isn't available (init failed), fall back to internal RAM so we
    // still get a picture rather than a black screen (k10video::blit no-ops on NULL).
    g_rgb = (uint16_t *)heap_caps_calloc(NES_W * NES_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_rgb)
        g_rgb = (uint16_t *)heap_caps_calloc(NES_W * NES_H, sizeof(uint16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[video] g_rgb=%p g_palette=%p free IRAM=%u bytes\n",
                  g_rgb, g_palette,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void blit(const uint8_t *bmp)
{
    if (!g_palette || !g_rgb) return;

    const uint8_t *src = bmp + 8;          // skip left overdraw column
    uint16_t *dst = g_rgb;
    for (int y = 0; y < NES_H; y++)
    {
        const uint8_t *row = src + y * NES_PITCH;
        for (int x = 0; x < NES_W; x++)
            *dst++ = g_palette[row[x]];
    }

    tft.startWrite();
    tft.setAddrWindow(OFFX, 0, NES_W, NES_H);
    // swap=true: our buffer is standard RGB565, ILI9341 wants it byte-swapped.
    // If red/blue look swapped, change the last arg to false.
    tft.pushColors(g_rgb, NES_W * NES_H, true);
    tft.endWrite();
}

TFT_eSPI& display()
{
    return tft;
}

} // namespace k10video
