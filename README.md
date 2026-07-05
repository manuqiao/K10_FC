K10开发板移植FC模拟器
=======
# UNIHIKER K10 — NES player (nofrendo, ported from retro-go)

Plays the NES ROMs bundled in `games/` on the DFRobot UNIHIKER K10, using the
[nofrendo](https://github.com/ducalex/retro-go/tree/master/retro-core/components/nofrendo)
emulator core from [ducalex/retro-go](https://github.com/ducalex/retro-go). On
boot a menu lists every game — pick one with **A** (move) and **B** (play).

The K10 is an ESP32-S3 (16 MB flash, 8 MB PSRAM) with a 2.8" **ILI9341**
320×240 (landscape) display, an I2S speaker amp, two buttons (A/B), and an
SC7A20H accelerometer — so the same ILI9341 + I2S + ESP32 recipe retro-go
uses on the ODROID-GO maps over almost directly.

The project is built with the **`unihiker-k10-platformio`** skill (PlatformIO /
Arduino). It builds clean with zero warnings; flash it and play.

---

## Controls

The K10 only has two buttons, so the D-pad comes from tilting the board:

| Action            | NES button | How                                 |
|-------------------|-----------|-------------------------------------|
| Move              | D-pad     | Tilt the board left/right/up/down   |
| Jump              | A         | Button **A**                        |
| Shoot             | B         | Button **B**                        |
| Start / Pause     | START     | **Shake** the board                 |
| Select            | SELECT    | Hold **A + B** together (~0.6 s)    |

To start Contra: tilt is movement, **shake** to get past the title screen.
Tuning (deadzone, axis, sign) is at the top of `src/k10_input.cpp`.

### Game-selection menu

On boot, before any game runs, the LCD shows the list of games found in `games/`:

| Action            | How                  |
|-------------------|----------------------|
| Move highlight    | Button **A** (wraps) |
| Start highlighted game | Button **B**     |

---

## Build / flash / monitor

Uses the bundled K10 PlatformIO (`~/K10P/pio`):

```bash
~/K10P/pio run                           # build
~/K10P/pio device list                   # find the K10's serial port
~/K10P/pio run -t upload --upload-port /dev/cu.usbmodemXXXX   # flash
~/K10P/pio device monitor                # serial @ 115200 (boot log + FPS info)
```

Every `.nes` file in `games/` is **embedded into firmware** at build time
(`tools/gen_rom_catalog.py` generates `src/rom_catalog{,_data}.h`), so a single
upload carries all games — no SD card, no separate filesystem step.

> If your K10 ever becomes unresponsive after an Arduino upload, use Mind+
> "Restore device initial settings" to recover, then re-flash.

---

## First-run tuning (only if something looks/sounds off)

These are one-line flips, each documented at the point of change:

- **Picture upside-down** → `src/k10_video.cpp`: change `tft.setRotation(1)` to `(3)`.
- **Red and blue swapped** → `src/k10_video.cpp`: change `tft.pushColors(..., true)` to `false`.
- **Tilt direction wrong / drift** → `src/k10_input.cpp`: adjust `TILT_DZ`, or
  flip `LR_SIGN` / `UD_SIGN`, or swap `AXIS_LR`/`AXIS_UD`.
- **Shake is unreliable for Start** → `src/k10_input.cpp`: swap the `Shake`→START
  line for an `A+B`→START mapping instead.

---

## Performance

Out of the box the SPI LCD runs at 40 MHz (the framework's TFT_eSPI default),
which gives a playable but not perfectly-smooth frame rate. If you want more
frames per second, bump the ILI9341 SPI clock to 80 MHz:

```
# K10 framework (shared on this machine):
# /Users/mac/K10P/.platformio/packages/framework-arduinounihiker/libraries/TFT_eSPI/User_Setup.h
#define SPI_FREQUENCY  80000000   # was 40000000
```

If 80 MHz shows speckle/tearing on your board, drop back to 40 MHz.

---

## Adding / changing games

Drop your `.nes` files into `games/` and rebuild — the build auto-discovers
every `*.nes` there, embeds them all, and the boot menu lists them in name order:

```bash
cp "MyGame.nes" games/
~/K10P/pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

To regenerate the catalog without a full build, run the generator directly
(`python3 tools/gen_rom_catalog.py`). Larger ROMs are fine — there is ~4 MB of
app-partition headroom (the full firmware with both sample ROMs fits in ~15% of
flash). The K10 has no D-pad hardware, so a different game's control feel may
vary.

---

## Project layout

```
platformio.ini            K10 env (Arduino, USB CDC, Model=None, PSRAM via board)
games/*.nes               ROM library — every .nes here is embedded at build time
tools/gen_rom_catalog.py  build-time generator (extra_scripts): scans games/*.nes
                          and writes src/rom_catalog{,_data}.h
src/
  main.cpp                boot, game menu, ROM load, NES frame loop
  k10_menu.{h,cpp}        boot game-selection screen (A=move, B=play)
  rom_catalog.h           generated: RomEntry + extern table
  rom_catalog_data.h      generated: the ROMs as PROGMEM arrays + table defs
  k10_video.{h,cpp}       NES palette → RGB565 → ILI9341 (TFT_eSPI @ landscape)
  k10_audio.{h,cpp}       I2S speaker output (BCLK0/WS38/DOUT45/MCLK3)
  k10_input.{h,cpp}       tilt (accelerometer) + A/B + shake → NES gamepad
lib/nofrendo/             retro-go nofrendo NES core; nes/utils.h is the only
                          file changed (standalone shim, no retro-go dependency)
```

## How the port works

- **Core**: retro-go's nofrendo is plain C; only `nes/utils.h` referenced
  retro-go (`rg_system.h` for logging/CRC32). That one file is replaced with a
  standalone shim, so the whole core compiles unmodified under Arduino.
- **Video**: the K10's panel is an ILI9341 driven by TFT_eSPI, exactly the
  controller retro-go targets. `k10video::blit` is installed as nofrendo's
  per-frame callback; it converts the 256-color indexed framebuffer through the
  RGB565 palette and pushes it to the LCD.
- **Audio**: `k10audio` reconfigures I2S_NUM_0 (the K10 pins) for speaker
  output and streams nofrendo's per-frame APU samples.
- **Input**: `k10input` maps the two buttons + tilt + shake to the NES pad and
  feeds it via nofrendo's `input_update()`.
- **Board**: `k10.begin()` brings up the I2C bus, GPIO expander (where the
  buttons/backlight/amp live), and accelerometer; the LCD is then driven
  directly for speed rather than through the LVGL canvas.

## Credits

- NES emulation: **nofrendo** © Matthew Conte, via [ducalex/retro-go](https://github.com/ducalex/retro-go) (GPL-2).
- Hardware: DFRobot UNIHIKER K10 / DFRobot platform-unihiker.
