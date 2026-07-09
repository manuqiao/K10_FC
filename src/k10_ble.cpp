#include "k10_ble.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLE_FFF0.h>
#include "k10_video.h"   // display()
#include "k10_input.h"   // read() — board buttons drive the pairing UI

// ---- NES pad bits (mirror k10_input.cpp's NP_* so this file is self-contained) ----
#define NP_A      0x01
#define NP_B      0x02
#define NP_SELECT 0x04
#define NP_START  0x08
#define NP_UP     0x10
#define NP_DOWN   0x20
#define NP_LEFT   0x40
#define NP_RIGHT  0x80

// Board buttons arrive through k10input packed into the NES byte (see
// k10_input.cpp): board A -> NP_B (0x02), board B -> NP_A (0x01), and holding
// A+B past BOTH_HOLD_MS asserts NP_SELECT (0x04). We reuse those for the picker.
#define BTN_NEXT    NP_B        // board A  -> cycle device
#define BTN_CONNECT NP_A        // board B  -> connect
#define BTN_RESCAN  NP_SELECT   // hold A+B -> rescan

namespace k10ble {

static BLE_FFF0 ble;

// Running NES-pad bitmask, updated by the BLE notify callback (Bluedroid task)
// and read by the frame loop. A single-byte store is atomic on Xtensa, so no
// lock is needed — same pattern as k10input::g_buttons.
static volatile uint8_t g_pad = 0;

// Controller key code (low 7 bits) -> NES pad bit. Index 0 unused; codes 1..8.
static const uint8_t CODE2NES[9] = {
    0,          // 0 unused
    NP_UP,      // 1 Up
    NP_DOWN,    // 2 Down
    NP_LEFT,    // 3 Left
    NP_RIGHT,   // 4 Right
    NP_A,       // 5 A
    NP_B,       // 6 B
    NP_SELECT,  // 7 Select
    NP_START,   // 8 Start
};

// Runs on the Bluedroid host task. A packet may carry several key events, so
// parse every byte. Only this callback writes g_pad, so a local shadow + single
// publish is safe.
static void onData(const uint8_t *data, size_t len)
{
    uint8_t np = g_pad;
    for (size_t i = 0; i < len; ++i)
    {
        uint8_t b    = data[i];
        uint8_t code = b & 0x7F;
        bool pressed = (b & 0x80) != 0;
        if (code < 1 || code > 8) continue;   // not a key event -> ignore
        uint8_t bit = CODE2NES[code];
        if (pressed) np |= bit;
        else         np &= ~bit;
    }
    g_pad = np;
}

static void onDisconnect()
{
    g_pad = 0;   // drop any held buttons so nothing stays stuck after a drop
}

void begin()
{
    static bool inited = false;
    if (inited) return;
    inited = true;

    ble.onData(onData);
    ble.onDisconnect(onDisconnect);
    ble.onLog([](const String &s) { Serial.println(s); });
    ble.begin("K10-NES");
    ble.setAutoReconnect(3);   // recover a mid-game drop (up to 3 tries)
}

uint8_t read()         { return g_pad; }
bool    isConnected()  { return ble.isConnected(); }

// ============================================================================
//  Scan / pick / connect UI (TFT_eSPI, driven by the board buttons)
// ============================================================================
static const int DISP_W   = 320;
static const int DISP_H   = 240;
static const int TITLE_Y  = 6;
static const int SEP_Y    = 28;
static const int TOP_Y    = 40;
static const int ROW_H    = 22;
static const int MAX_ROWS = 7;
static const int FOOTER_Y = DISP_H - 20;

static void drawScanning(TFT_eSPI &tft)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  BLE Scanning...", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Searching for BLE devices (2s)", 8, TOP_Y + 10);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("See serial for live scan results", 8, TOP_Y + 40);
}

// `order` maps a display row -> ble.device() index (sorted by RSSI desc).
static void drawPicker(TFT_eSPI &tft, const int *order, int count,
                       int sel, int top)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextWrap(false);
    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  Select Device", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);

    int rows = count < MAX_ROWS ? count : MAX_ROWS;
    for (int i = 0; i < rows; i++)
    {
        int idx = top + i;
        if (idx >= count) break;
        int y = TOP_Y + i * ROW_H;
        const BLE_FFF0::Device *d = ble.device(order[idx]);
        bool hot = (idx == sel);

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
        const char *nm = (d && d->name.length()) ? d->name.c_str()
                                                 : (d ? d->address.c_str() : "?");
        char line[48];
        snprintf(line, sizeof(line), "%-22s %ddB", nm, d ? d->rssi : 0);
        tft.drawString(line, 24, y + 3);
    }

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("A:next  B:connect  hold A+B:rescan", 8, FOOTER_Y);
}

static void drawConnecting(TFT_eSPI &tft, const char *name)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Connecting...", 8, TOP_Y + 10);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(name, 8, TOP_Y + 40);
}

static void drawMessage(TFT_eSPI &tft, uint16_t color,
                        const char *l1, const char *l2)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextFont(2);
    tft.setTextColor(color, TFT_BLACK);
    tft.drawString(l1, 8, TOP_Y + 10);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(l2, 8, TOP_Y + 40);
}

bool connect_flow()
{
    TFT_eSPI &tft = k10video::display();

    for (;;)
    {
        // ---- scan (blocking 2 s) ----
        drawScanning(tft);
        ble.scan(2000);

        int count = ble.deviceCount();
        if (count == 0)
        {
            drawMessage(tft, TFT_RED, "No BLE devices found", "Rescanning in 2s...");
            delay(2000);
            continue;
        }

        // Sort device indices by RSSI (strongest first). Cap the list so a noisy
        // 2.4 GHz environment doesn't overflow the stack array.
        int order[32];
        if (count > 32) count = 32;
        for (int i = 0; i < count; i++) order[i] = i;
        for (int i = 0; i < count - 1; i++)
            for (int j = i + 1; j < count; j++)
                if (ble.device(order[j])->rssi > ble.device(order[i])->rssi)
                { int t = order[i]; order[i] = order[j]; order[j] = t; }

        // ---- pick (board buttons) ----
        int sel = 0, top = 0;
        drawPicker(tft, order, count, sel, top);
        uint8_t prev = k10input::read();

        int chosen = -1;          // ble.device() index to connect to
        for (;;)
        {
            uint8_t cur = k10input::read();
            bool next_edge   = (cur & BTN_NEXT)    && !(prev & BTN_NEXT);
            bool conn_edge   = (cur & BTN_CONNECT) && !(prev & BTN_CONNECT);
            bool rescan_edge = (cur & BTN_RESCAN)  && !(prev & BTN_RESCAN);
            prev = cur;

            if (rescan_edge) break;   // -> outer loop rescans

            if (next_edge && count > 1)
            {
                sel = (sel + 1) % count;
                if (sel < top) top = sel;
                else if (sel >= top + MAX_ROWS) top = sel - MAX_ROWS + 1;
                drawPicker(tft, order, count, sel, top);
            }
            if (conn_edge) { chosen = order[sel]; break; }

            delay(20);
        }
        if (chosen < 0) { delay(150); continue; }   // rescan requested

        // ---- connect (uses the addrType captured during scan) ----
        const BLE_FFF0::Device *d = ble.device(chosen);
        const char *name = (d && d->name.length()) ? d->name.c_str()
                         : (d ? d->address.c_str() : "?");
        drawConnecting(tft, name);

        if (ble.connect(chosen))
        {
            drawMessage(tft, TFT_GREEN, "Connected:", name);
            delay(700);
            return true;
        }
        drawMessage(tft, TFT_RED, "Connect failed", "Retrying scan in 2s...");
        delay(2000);
        // loop -> fresh scan
    }
}

} // namespace k10ble
