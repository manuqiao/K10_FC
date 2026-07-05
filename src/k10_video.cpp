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

// NOTE on the GDMA path: an earlier attempt used tft.initDMA() +
// pushPixelsDMA() to overlap the SPI transfer with emulation. It produced a
// black screen: on this DFRobot TFT_eSPI fork, spi_bus_initialize() (called by
// initDMA) re-routes the SPI2 GPIO matrix in a way that breaks TFT_eSPI's
// direct-register writes (fillScreen/drawString/setAddrWindow/pushColors) — so
// even the boot menu stopped drawing. GDMA is therefore NOT usable here unless
// the whole interface moves to the IDF driver (esp_lcd). We instead use an
// optimised blocking path (below): cheaper than the original, no DMA risk.
// Real async would need a core-0 blit worker + double-buffered vidbuf.
//
// Set to 0 to fall back to the original PSRAM full-frame + swap=true path.
#define K10_BLIT_OPTIMIZED 1

static TFT_eSPI tft = TFT_eSPI();
static uint16_t *g_palette = nullptr;   // 256 RGB565

#if K10_BLIT_OPTIMIZED
// Optimised blocking path: convert one strip at a time into an internal-RAM
// buffer (avoids the original's 120 KiB PSRAM round-trip) and push with no
// per-pixel byte swap (palette is pre-swapped at init). The ILI9341 keeps its
// GRAM write pointer across the CS toggles that pushColors() emits between
// strips, so a single setAddrWindow() + N pushColors() writes the full frame.
#define STRIP_H    80                     // 240/80 = 3 strips; 256*80*2 = 40 KiB buffer
#define STRIP_PX   (NES_W * STRIP_H)
#define NUM_STRIPS (NES_H / STRIP_H)      // 3
static uint16_t *g_strip = nullptr;       // reuse buffer (internal RAM; PSRAM fallback)
#else
static uint16_t *g_rgb = nullptr;         // NES_W*NES_H RGB565 scratch (PSRAM)
#endif

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

#if K10_BLIT_OPTIMIZED
    // Store the palette already byte-swapped into ILI9341 wire order, so blit
    // can push with swap=false (no per-pixel swap). This reproduces exactly
    // what pushColors(..., swap=true) put on the wire. If red/blue look swapped
    // on screen, drop this loop (or invert it) and pass swap=true to pushColors.
    for (int i = 0; i < 256; i++)
    {
        uint16_t v = g_palette[i];
        g_palette[i] = (v << 8) | (v >> 8);
    }

    // One reuse strip buffer in DMA-capable internal RAM. Fall back to PSRAM if
    // internal is tight (still correct, just slower — pushColors reads it by CPU).
    g_strip = (uint16_t *)heap_caps_malloc(NES_W * STRIP_H * 2,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_strip)
        g_strip = (uint16_t *)heap_caps_malloc(NES_W * STRIP_H * 2, MALLOC_CAP_SPIRAM);
    Serial.printf("[video] optimised blit  g_strip=%p (internal=%d)  g_palette=%p  free IRAM=%u bytes\n",
                  g_strip,
                  g_strip ? (int)!!(heap_caps_get_allocated_size(g_strip) & MALLOC_CAP_INTERNAL) : 0,
                  g_palette,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    // 256*240*2 = ~120 KiB -> keep it in PSRAM so internal RAM stays free.
    // If PSRAM isn't available (init failed), fall back to internal RAM so we
    // still get a picture rather than a black screen (blit no-ops on NULL).
    g_rgb = (uint16_t *)heap_caps_calloc(NES_W * NES_H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_rgb)
        g_rgb = (uint16_t *)heap_caps_calloc(NES_W * NES_H, sizeof(uint16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[video] original blit  g_rgb=%p g_palette=%p free IRAM=%u bytes\n",
                  g_rgb, g_palette,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#endif
}

void blit(const uint8_t *bmp)
{
#if K10_BLIT_OPTIMIZED
    if (!g_palette || !g_strip) return;

    // setAddrWindow() once for the whole frame; pushColors() between strips
    // toggles CS but the ILI9341 retains the GRAM write pointer, so successive
    // data bursts keep filling consecutive addresses (no per-strip window).
    tft.startWrite();
    tft.setAddrWindow(OFFX, 0, NES_W, NES_H);

    const uint8_t *src = bmp + 8;            // skip left overdraw column
    for (int s = 0; s < NUM_STRIPS; s++)
    {
        uint16_t *dst = g_strip;
        const uint8_t *rowbase = src + (s * STRIP_H) * NES_PITCH;
        for (int y = 0; y < STRIP_H; y++)
        {
            const uint8_t *r = rowbase + y * NES_PITCH;
            uint16_t *d = dst + y * NES_W;
            for (int x = 0; x < NES_W; x++)
                d[x] = g_palette[r[x]];
        }
        // swap=false: palette is pre-swapped into ILI wire order.
        tft.pushColors(dst, STRIP_PX, false);
    }
    tft.endWrite();
#else
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
#endif
}

TFT_eSPI& display()
{
    return tft;
}

} // namespace k10video
