#include "k10_ble_hid.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLE_HID_Host.h>
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

namespace k10blehid {

static BLE_HID_Host hid;

// Running NES-pad bitmask, updated by the HID notify callback (Bluedroid task)
// and read by the frame loop. A single-byte store is atomic on Xtensa, so no
// lock is needed — same pattern as k10input::g_buttons / k10ble::g_pad.
static volatile uint8_t g_pad = 0;

// ---- Button-number -> NES bit mapping -------------------------------------
// BLE HID gamepads expose Button 1..N (Usage Page 0x09), NOT A/B/Start/Select.
// The Android/XInput convention (Linux gamepad spec) is used by default:
//
//   Button 1 = A         Button 5 = L1        Button 9  = Home/Guide
//   Button 2 = B         Button 6 = R1        Button 10 = L3
//   Button 3 = X (spare) Button 7 = Select    Button 11 = R3
//   Button 4 = Y (spare) Button 8 = Start     Button 12/13 = L2/R2
//
// But cheap pads vary wildly. This table is THE tuning point: watch the serial
// `[hid] rpt ...` / `[hid] btn=... held=[...]` lines (K10_BLE_HID_DEBUG=1 in
// libs/BLE_HID_Host/BLE_HID_Host.cpp) for your pad's raw bytes and remap here.
// Index i = Button (i+1); 0 = unused.
//
// MEASURED for the current pad (10-byte reportId-4 reports, no leading ID byte):
//   face A -> Btn 2   face B -> Btn 1   Start -> Btn 12   Select -> Btn 11
// The D-pad arrives via the hat (Usage 0x39), mapped in hatToNES(), not here.
static const uint8_t BUTTON2NES[16] = {
    /* Btn 1  */ NP_B,        // face B
    /* Btn 2  */ NP_A,        // face A
    /* Btn 3  */ 0,
    /* Btn 4  */ 0,
    /* Btn 5  */ 0,
    /* Btn 6  */ 0,
    /* Btn 7  */ 0,
    /* Btn 8  */ 0,
    /* Btn 9  */ 0,
    /* Btn 10 */ 0,
    /* Btn 11 */ NP_SELECT,  // Select
    /* Btn 12 */ NP_START,   // Start
    /* Btn 13 */ 0,
    /* Btn 14 */ 0,
    /* Btn 15 */ 0,
    /* Btn 16 */ 0,
};

// ---- D-pad hat (Usage 0x39, 4-bit) -> NES direction bits ------------------
// 0=N 1=NE 2=E 3=SE 4=S 5=SW 6=W 7=NW; 8..15 / 0xFF = released.
static uint8_t hatToNES(uint8_t hat)
{
    switch (hat)
    {
        case 0: return NP_UP;
        case 1: return NP_UP | NP_RIGHT;
        case 2: return NP_RIGHT;
        case 3: return NP_DOWN | NP_RIGHT;
        case 4: return NP_DOWN;
        case 5: return NP_DOWN | NP_LEFT;
        case 6: return NP_LEFT;
        case 7: return NP_UP | NP_LEFT;
        default: return 0;   // released / no hat
    }
}

// Runs on the Bluedroid host task. Map the decoded gamepad state -> NES byte.
// Only this callback writes g_pad, so a local build + single publish is safe.
//
// Edge-triggered serial print: a gamepad spams reports even while idle, so we
// only log when the raw button/hat state actually changes. Output shows the raw
// held buttons (their 1-based HID button numbers), the hat value, and the
// decoded NES byte — this is what the BUTTON2NES comment above refers to: read
// these button numbers off the serial line to tune the mapping for your pad.
// Set K10_BLE_HID_PRINT=0 to silence once tuned.
#ifndef K10_BLE_HID_PRINT
#define K10_BLE_HID_PRINT 1
#endif

// NES-bit mask -> short label, for a readable decoded line.
static const char *nesLabel(uint8_t bit)
{
    switch (bit)
    {
        case NP_A:      return "A";
        case NP_B:      return "B";
        case NP_SELECT: return "SELECT";
        case NP_START:  return "START";
        case NP_UP:     return "UP";
        case NP_DOWN:   return "DOWN";
        case NP_LEFT:   return "LEFT";
        case NP_RIGHT:  return "RIGHT";
        default:        return "?";
    }
}

static void printReport(const BLE_HID_Host::GamepadState& st, uint8_t np)
{
    Serial.printf("[hid] btn=0x%04X", (unsigned)st.buttons);
    Serial.print(" held=[");
    bool first = true;
    for (uint8_t b = 0; b < 16; b++)
        if (st.buttons & (1UL << b))
        {
            Serial.printf("%s%d", first ? "" : ",", b + 1);   // 1-based HID button no.
            first = false;
        }
    Serial.printf("] hat=%d  ->  nes=0x%02X [", (int)st.hat, np);
    first = true;
    if (np & NP_A)      { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_A));      first = false; }
    if (np & NP_B)      { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_B));      first = false; }
    if (np & NP_SELECT) { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_SELECT)); first = false; }
    if (np & NP_START)  { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_START));  first = false; }
    if (np & NP_UP)     { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_UP));     first = false; }
    if (np & NP_DOWN)   { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_DOWN));   first = false; }
    if (np & NP_LEFT)   { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_LEFT));   first = false; }
    if (np & NP_RIGHT)  { Serial.printf("%s%s", first ? "" : "|", nesLabel(NP_RIGHT));  first = false; }
    Serial.println("]");
}

static void onReport(const BLE_HID_Host::GamepadState& st)
{
    uint8_t np = 0;
    for (uint8_t b = 0; b < 16; b++)
        if (st.buttons & (1UL << b))
            np |= BUTTON2NES[b];
    np |= hatToNES(st.hat);
    g_pad = np;

#if K10_BLE_HID_PRINT
    // Edge-trigger: only print when the raw HID state changes, not every report.
    static uint32_t lastBtn = 0xFFFFFFFF;   // start impossible so first report prints
    static uint8_t  lastHat = 0xFF;
    if (st.buttons != lastBtn || st.hat != lastHat)
    {
        lastBtn = st.buttons;
        lastHat = st.hat;
        printReport(st, np);
    }
#endif
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

    hid.onReport(onReport);
    hid.onDisconnect(onDisconnect);
    hid.onLog([](const String &s) { Serial.println(s); });
    hid.begin("K10-NES-HID");
    hid.setAutoReconnect(3);   // recover a mid-game drop (up to 3 tries)
}

uint8_t read()        { return g_pad; }
bool    isConnected() { return hid.isConnected(); }

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
    tft.drawString("K10 NES  -  BLE HID Scanning...", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("Searching for HID gamepads (2s)", 8, TOP_Y + 10);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("See serial for live scan results", 8, TOP_Y + 40);
}

// `order` maps a display row -> hid.device() index (sorted by RSSI desc).
static void drawPicker(TFT_eSPI &tft, const int *order, int count,
                       int sel, int top)
{
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);
    tft.setTextWrap(false);
    tft.setTextFont(2);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("K10 NES  -  Select HID Device", 8, TITLE_Y);
    tft.drawFastHLine(0, SEP_Y, DISP_W, TFT_DARKGREY);

    int rows = count < MAX_ROWS ? count : MAX_ROWS;
    for (int i = 0; i < rows; i++)
    {
        int idx = top + i;
        if (idx >= count) break;
        int y = TOP_Y + i * ROW_H;
        const BLE_HID_Host::Device *d = hid.device(order[idx]);
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
    tft.drawString("Connecting (pairing)...", 8, TOP_Y + 10);
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
        hid.scan(2000);

        int count = hid.deviceCount();
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
                if (hid.device(order[j])->rssi > hid.device(order[i])->rssi)
                { int t = order[i]; order[i] = order[j]; order[j] = t; }

        // ---- pick (board buttons) ----
        int sel = 0, top = 0;
        drawPicker(tft, order, count, sel, top);
        uint8_t prev = k10input::read();

        int chosen = -1;          // hid.device() index to connect to
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

        // ---- connect (bonds + subscribes; may take a couple seconds) ----
        const BLE_HID_Host::Device *d = hid.device(chosen);
        const char *name = (d && d->name.length()) ? d->name.c_str()
                         : (d ? d->address.c_str() : "?");
        drawConnecting(tft, name);

        if (hid.connect(chosen))
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

} // namespace k10blehid
