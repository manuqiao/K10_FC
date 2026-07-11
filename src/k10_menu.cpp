#include "k10_menu.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "k10_video.h"   // display()
#include "k10_input.h"   // read() — the boot mode-select is always board-driven

// ---- NES pad bits (mirror k10_input.cpp's NP_*) ----
#define NP_A      0x01
#define NP_B      0x02
#define NP_START  0x08
#define NP_UP     0x10
#define NP_DOWN   0x20

// k10input packs the two physical K10 buttons into the NES pad byte (see
// k10_input.cpp): board B -> NES A (0x01), board A -> NES B (0x02). So for the
// board-driven screens: board A (NP_B) = move/toggle, board B (NP_A) = confirm.

namespace k10menu {

static const int DISP_W = 320;
static const int DISP_H = 240;

static const int TITLE_Y   = 6;
static const int SEP_Y     = 28;
static const int TOP_Y     = 38;     // first row's top
static const int ROW_H     = 26;
static const int FOOTER_Y  = DISP_H - 20;

// Redraw the whole menu. `sel` is the highlighted index; `top` is the first
// visible row (windowed scrolling when there are more games than fit).
static void draw_menu(TFT_eSPI &tft, const RomEntry *roms, size_t count,
                      int sel, int top, uint16_t sel_bg, uint16_t sel_fg)
{
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(TL_DATUM);
    tft.setTextWrap(false);

    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  Select Game", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);

    int max_rows = (FOOTER_Y - TOP_Y) / ROW_H;
    int rows = (int)count < max_rows ? (int)count : max_rows;
    for (int i = 0; i < rows; i++)
    {
        int idx = top + i;
        if (idx >= (int)count) break;
        int y = TOP_Y + i * ROW_H;
        bool hot = (idx == sel);

        if (hot)
        {
            tft.fillRect(0, y - 1, DISP_W, ROW_H, sel_bg);
            tft.setTextColor(sel_fg, sel_bg);
            tft.drawString(">", 6, y + 3);
        }
        else
        {
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
        }
        tft.drawString(roms[idx].name, 24, y + 3);
    }

    // scroll arrows when the list is longer than the window
    if (count > (size_t)max_rows)
    {
        tft.setTextFont(2);
        tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
        if (top > 0)                 tft.drawString("^", DISP_W - 16, TOP_Y - 4);
        if (top + max_rows < (int)count) tft.drawString("v", DISP_W - 16, FOOTER_Y - ROW_H);
    }

    tft.setTextFont(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("A: move    B: play", 8, FOOTER_Y);
}

ControlMode select_mode()
{
    TFT_eSPI &tft = k10video::display();
    const int N = 5;
    const char *labels[N] = { "Local control  (board)",
                              "Bluetooth controller  (custom)",
                              "Bluetooth HID gamepad",
                              "Matrix keypad",
                              "ADKeyboard  (extender C0)" };

    auto draw = [&](int sel) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        tft.setTextWrap(false);
        tft.setTextFont(2);
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.drawString("K10 NES  -  Control Mode", 8, TITLE_Y);
        tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);

        for (int i = 0; i < N; i++)
        {
            int y = TOP_Y + 18 + i * (ROW_H + 4);
            bool hot = (i == sel);
            if (hot)
            {
                tft.fillRect(0, y - 1, DISP_W, ROW_H, TFT_NAVY);
                tft.setTextColor(TFT_YELLOW, TFT_NAVY);
                tft.drawString(">", 6, y + 3);
            }
            else
            {
                tft.setTextColor(TFT_WHITE, TFT_BLACK);
            }
            tft.drawString(labels[i], 24, y + 3);
        }

        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        tft.drawString("A:choose   B:confirm", 8, FOOTER_Y);
    };

    int sel = 0;
    draw(sel);
    uint8_t prev = k10input::read();
    for (;;)
    {
        uint8_t cur = k10input::read();
        bool toggle  = (cur & NP_B) && !(prev & NP_B);   // board A
        bool confirm = (cur & NP_A) && !(prev & NP_A);   // board B
        prev = cur;

        if (toggle) { sel = (sel + 1) % N; draw(sel); }  // cycle through 5 modes
        if (confirm)
        {
            draw(sel); delay(140);   // brief flash so the pick registers
            return (ControlMode)sel; // 0=LOCAL, 1=BLUETOOTH(custom), 2=BT_HID, 3=MATRIX, 4=ADKEYBOARD
        }
        delay(20);
    }
}

int select_game(const RomEntry *roms, size_t count, InputFn read, bool dpadNav)
{
    // Zero-ROM safety net; main.cpp guards this too, but don't deref if it slips.
    if (count == 0) return -1;

    // Navigation masks per source. The board has no D-pad and tilt would bleed
    // into D-pad bits, so for the board we navigate strictly on the button bits
    // (board A = NP_B -> next, board B = NP_A -> confirm) — unchanged from
    // before. A BLE controller has a real D-pad, so Up/Down move and A/Start
    // confirm.
    uint8_t nextMask, prevMask, confirmMask;
    if (dpadNav)
    {
        nextMask    = NP_DOWN;
        prevMask    = NP_UP;
        confirmMask = NP_A | NP_START;
    }
    else
    {
        nextMask    = NP_B;          // board A -> cycle row
        prevMask    = 0;             // board has no "previous"
        confirmMask = NP_A;          // board B -> play
    }

    TFT_eSPI &tft = k10video::display();
    int sel = 0;
    int top = 0;

    draw_menu(tft, roms, count, sel, top, TFT_NAVY, TFT_YELLOW);

    uint8_t prev = read();
    for (;;)
    {
        uint8_t cur = read();
        bool next_edge    = (cur & nextMask)    && !(prev & nextMask);
        bool prev_edge    = prevMask && (cur & prevMask) && !(prev & prevMask);
        bool confirm_edge = (cur & confirmMask) && !(prev & confirmMask);
        prev = cur;

        int max_rows = (FOOTER_Y - TOP_Y) / ROW_H;

        // Check confirm first: a simultaneous move+confirm picks the current row
        // instead of also advancing it.
        if (confirm_edge)
        {
            // Brief inverted flash so the user sees the pick registered.
            draw_menu(tft, roms, count, sel, top, TFT_YELLOW, TFT_BLACK);
            delay(140);
            return sel;
        }

        if (next_edge && count > 1)
        {
            sel = (sel + 1) % (int)count;
            if (sel < top) top = sel;
            else if (sel >= top + max_rows) top = sel - max_rows + 1;
            draw_menu(tft, roms, count, sel, top, TFT_NAVY, TFT_YELLOW);
        }

        if (prev_edge && count > 1)
        {
            sel = (sel - 1 + (int)count) % (int)count;
            if (sel < top) top = sel;
            else if (sel >= top + max_rows) top = sel - max_rows + 1;
            draw_menu(tft, roms, count, sel, top, TFT_NAVY, TFT_YELLOW);
        }

        delay(20);   // input task polls ~every 10 ms; this is plenty responsive
    }
}

} // namespace k10menu
