/*
 * UNIHIKER K10 — NES player (nofrendo core, ported from ducalex/retro-go).
 *
 * Every .nes in the project's games/ folder is embedded into firmware at build
 * time (see tools/gen_rom_catalog.py). On boot a game-selection menu lists them;
 * pick one with board A (move) + board B (play), and the NES core runs it.
 *
 *   Build:    pio run
 *   Upload:   pio run -t upload --upload-port /dev/cu.usbmodem****
 *   Monitor:  pio device monitor
 */

#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_log.h"       // esp_log_level_set — mute the noisy ESP-IDF gpio tag
#include "unihiker_k10.h"
#include "initBoard.h"
#include "k10_video.h"
#include "k10_audio.h"
#include "k10_input.h"
#include "k10_matrix.h"
#include "k10_adkey.h"
#include "k10_menu.h"
#include "k10_ble_hid.h"
#include "rom_catalog.h"       // RomEntry, g_roms, g_rom_count (extern)
#include "rom_catalog_data.h"  // the embedded ROM images + table (one TU only)

extern "C" {
#include "nofrendo.h"      // nofrendo_init
#include "nes/nes.h"       // nes_t, nes_getptr, nes_setvidbuf, nes_emulate, ...
#include "nes/input.h"     // input_connect, input_update, NES_JOYPAD
#include "nes/rom.h"       // rom_t, rom_loadmem, nes_insertcart
}

UNIHIKER_K10 k10;

static const int AUDIO_RATE = 32000;

// Run the NES at full 60 Hz for audio, but blit/render only every Nth frame.
// The blit is a CPU-blocking SPI push (k10_video.cpp K10_BLIT_OPTIMIZED=1:
// pre-swapped palette + internal-RAM strip + no-swap pushColors). A GDMA/async
// path was tried but black-screened on this DFRobot TFT_eSPI fork (initDMA
// breaks the direct-register writes the menu/blit rely on) — see k10_video.cpp.
// Measured-ish: a rendered frame is emu≈12ms + blit≈16ms ≈ 28ms; a skipped
// frame (draw=false) is ~7ms. N must keep the average < 16666us so the emulator
// outpaces realtime and submit()'s backpressure paces the loop to exactly 60 Hz
// (that's what keeps audio smooth). N=2 averages ~17.5ms (breaks audio); N=3
// averages ~14ms (OK, ~2.5ms headroom) -> video ~20fps, audio 60Hz, less stutter
// than the old PSRAM+swap blit (which averaged ~15.7ms with only ~1ms headroom).
// Raising video fps needs real async (core-0 blit worker + double-buffered
// vidbuf), not a smaller N.
// Audio is produced every frame regardless (apu_emulate() runs even if draw=false).
static const int BLIT_EVERY_N = 3;

// Per-frame timing probe: prints a [perf] line every 60 frames. Set 0 to silence
// (e.g. while reading HID debug on the serial console). Accumulators still run.
#define PERF_DEBUG 0

static nes_t   *g_nes = nullptr;
static uint8_t *g_vidbuf = nullptr;            // nofrendo indexed framebuffer

// True once the player picked the Bluetooth HID gamepad. loop() then reads k10blehid.
static bool g_bt_hid_mode = false;

// True once the player picked the matrix keypad. loop() then reads k10matrix.
static bool g_matrix_mode = false;

// True once the player picked the ADKeyboard (Gravity A0). loop() then reads k10adkey.
static bool g_adkey_mode = false;

// Per-frame timing probe: accumulated across 60 frames, then printed. g_blit_us
// is filled by nes_blit_cb during nes_emulate() so we can separate blit cost
// from emulation cost. g_blit_cnt counts how many times the blit callback
// actually fired (0 => video isn't rendering, likely a NULL vidbuf).
static uint32_t g_blit_us = 0;
static uint32_t g_blit_cnt = 0;

// nofrendo calls this once per frame with the just-rendered indexed buffer.
extern "C" void nes_blit_cb(uint8_t *bmp)
{
    uint32_t t = micros();
    k10video::blit((const uint8_t *)bmp);
    g_blit_us += micros() - t;
    g_blit_cnt++;
}

static bool load_rom(const uint8_t *data, size_t len, const char *name)
{
    // ROM images are flash-backed (PROGMEM); rom_loadmem parses its own copy.
    rom_t *cart = rom_loadmem((uint8_t *)data, len);
    if (!cart)
    {
        Serial.println("[rom] rom_loadmem failed");
        return false;
    }
    if (nes_insertcart(cart) < 0)
    {
        Serial.println("[rom] nes_insertcart failed (unsupported mapper?)");
        return false;
    }
    Serial.printf("[rom] \"%s\" loaded (%u bytes)\n", name, (unsigned)len);
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    // Mute the ESP-IDF gpio driver's spammy `E (xxx) gpio: ...` log lines — it
    // drops harmless pin-config warnings on this BSP and drowns out [adkey]/
    // [perf]. Only the "gpio" tag is silenced; everything else is untouched.
    // Change to ESP_LOG_ERROR (or ESP_LOG_WARN) here if you ever want real gpio
    // faults back.
    esp_log_level_set("gpio", ESP_LOG_NONE);
    Serial.println("\n=== K10 NES (nofrendo) ===");

    // I2C bus, GPIO expander, buttons, accelerometer, I2S bus, RGB.
    k10.begin();
    delay(50);

    k10video::init();             // ILI9341 landscape + palette + backlight
    k10input::init();             // background button/tilt poller
    k10matrix::init();            // external 4x2 keypad: rows P2/P3/P8/P13, cols P0/P1,
                                  // logs [matrix] pressed/released to Serial

    // ---- Game-selection menu (before bringing up the emulator) ----
    if (g_rom_count == 0)
    {
        Serial.println("[main] no .nes files in games/ -- nothing to play. Halting.");
        k10video::display().fillScreen(TFT_BLACK);
        k10video::display().setTextColor(TFT_RED, TFT_BLACK);
        k10video::display().drawString("No games in games/", 8, 8, 2);
        while (true) delay(1000);
    }

    Serial.printf("[menu] %u game(s) available\n", (unsigned)g_rom_count);

    // ---- Control-mode choice (board A = choose, board B = confirm) ----
    k10menu::ControlMode mode = k10menu::select_mode();

    k10menu::InputFn input = k10input::read;   // default: board (tilt + 2 buttons)
    bool    dpadNav = false;
    if (mode == k10menu::MODE_BT_HID)
    {
        Serial.println("[main] BLE HID mode: scanning for a gamepad...");
        k10blehid::begin();
        if (!k10blehid::connect_flow())   // blocks until bonded (loops on failure)
        {
            Serial.println("[main] BLE HID pairing failed; halting");
            while (true) delay(1000);
        }
        input   = k10blehid::read;
        dpadNav = true;                // gamepad has a real D-pad
        g_bt_hid_mode = true;
        Serial.println("[main] HID gamepad connected; control handed to BLE HID");
    }
    else if (mode == k10menu::MODE_MATRIX)
    {
        // Matrix keypad takes over. The board input task is unused in this mode,
        // so suspend it: its slow expander reads were contending for the I2C bus
        // and caused the ~2s key lag. Freeing the bus lets the matrix scan fast.
        k10input::suspend();
        input   = k10matrix::read;
        dpadNav = true;                // matrix has a real D-pad
        g_matrix_mode = true;
        Serial.println("[main] matrix keypad mode; board poller suspended");
    }
    else if (mode == k10menu::MODE_ADKEYBOARD)
    {
        // ADKeyboard on the board's own Gravity analog port (native ESP32 ADC1 via
        // analogRead — no I2C). It still reads board A/B on the I2C expander for
        // SELECT/START, though, so suspend the board poller AND the matrix scan to
        // keep the shared Wire bus uncontended (Wire isn't thread-safe between
        // tasks). k10adkey borrows board A/B itself, so the board poller isn't
        // needed here either.
        k10input::suspend();
        k10matrix::suspend();
        k10adkey::init();
        input   = k10adkey::read;
        dpadNav = true;                // ADKeyboard has a real D-pad (s2/s3/s4/s5)
        g_adkey_mode = true;
        Serial.println("[main] ADKeyboard mode (Gravity A0); board poller + matrix suspended");
    }

    int chosen = k10menu::select_game(g_roms, g_rom_count, input, dpadNav);
    if (chosen < 0 || chosen >= (int)g_rom_count)
    {
        Serial.println("[main] menu returned no selection; halting");
        while (true) delay(1000);
    }
    const RomEntry &game = g_roms[chosen];
    Serial.printf("[menu] selected: %s\n", game.name);

    // Clear the menu off the screen so its text doesn't linger in the side bars
    // (the NES blit only repaints the central 256x240).
    k10video::display().fillScreen(TFT_BLACK);

    // Create NES instance (CPU/PPU/APU/mem), install our blit callback.
    nofrendo_init(SYS_DETECT, AUDIO_RATE, /*stereo=*/false,
                  (void *)nes_blit_cb, NULL, NULL);
    g_nes = nes_getptr();

    k10audio::init(AUDIO_RATE);   // reconfigure I2S for speaker output

    input_connect(0, NES_JOYPAD);
    g_vidbuf = (uint8_t *)heap_caps_calloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT,
                                           1, MALLOC_CAP_SPIRAM);
    if (!g_vidbuf)   // PSRAM unavailable/uninitialised -> fall back to internal RAM
        g_vidbuf = (uint8_t *)heap_caps_calloc(NES_SCREEN_PITCH * NES_SCREEN_HEIGHT,
                                               1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[init] g_vidbuf=%p  free PSRAM=%u  free IRAM=%u bytes  "
                  "(NULL => no video; PSRAM=0 means PSRAM init failed)\n",
                  g_vidbuf,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    if (!load_rom(game.data, game.len, game.name))
    {
        Serial.println("[main] halting");
        while (true) delay(1000);
    }

    // Must install the video buffer AFTER load_rom(): nes_insertcart() calls
    // nes_reset(), which zeroes nes.vidbuf — so an earlier nes_setvidbuf() is
    // silently undone and the screen stays black (the blit callback never fires
    // because nes_emulate() forces draw=false when vidbuf==NULL).
    nes_setvidbuf(g_vidbuf);
    Serial.printf("[init] vidbuf installed=%p\n", g_vidbuf);

    if (g_bt_hid_mode)
        Serial.println("[main] running. HID gamepad: D-pad=move  A/B=buttons  Start/Select as labelled");
    else if (g_matrix_mode)
        Serial.println("[main] running. Matrix: D-pad=move  A/B=buttons  Start/Select as labelled");
    else if (g_adkey_mode)
        Serial.println("[main] running. ADKeyboard: s2/s4/s3/s5=D-pad  s1=A  board A=Select  board B=Start");
    else
        Serial.println("[main] running. Tilt=move  B=jump(A)  A=shoot(B)  Cover-light=start  Hold A+B=select");
}

void loop()
{
    static uint32_t acc_in = 0, acc_em = 0, acc_au = 0, acc_blit = 0, acc_blit_cnt = 0;
    static uint32_t frames = 0, frame_no = 0;

    uint32_t t0 = micros();
    uint8_t buttons;
    if (g_bt_hid_mode)
        // BLE HID gamepad: feed 0 while the link is down so a dropped gamepad
        // doesn't leave buttons stuck; auto-reconnect brings it back.
        buttons = k10blehid::isConnected() ? k10blehid::read() : 0;
    else if (g_matrix_mode)
        buttons = k10matrix::read();
    else if (g_adkey_mode)
        buttons = k10adkey::read();
    else
        buttons = k10input::read();                // instant: cached by input task
    input_update(0, buttons);                          // feed joypad state
    uint32_t t1 = micros();

    // Render/blit every Nth frame; audio (apu_emulate) runs every frame either way.
    bool draw = (frame_no++ % BLIT_EVERY_N == 0);
    g_blit_us = 0;
    g_blit_cnt = 0;
    nes_emulate(draw);                                 // run one frame (renders + blits + audio)
    uint32_t t2 = micros();
    acc_blit += g_blit_us;
    acc_blit_cnt += g_blit_cnt;

    k10audio::submit(g_nes->apu->buffer, g_nes->apu->samples_per_frame);
    uint32_t t3 = micros();

    acc_in += t1 - t0;
    acc_em += (t2 - t1) - g_blit_us;                   // emulation, excluding blit
    acc_au += t3 - t2;                                  // audio: ~0 when decoupled & keeping up

    if (++frames >= 60)
    {
#if PERF_DEBUG
        Serial.printf("[perf] avg us/frame: input=%lu  emu=%lu  blit=%lu  audio=%lu  "
                      "TOTAL=%lu  (budget 16666)  blits=%lu/60\n",
                      acc_in / 60, acc_em / 60, acc_blit / 60, acc_au / 60,
                      (acc_in + acc_em + acc_blit + acc_au) / 60, acc_blit_cnt);
#endif
        acc_in = acc_em = acc_au = acc_blit = 0;
        acc_blit_cnt = 0;
        frames = 0;
    }
}
