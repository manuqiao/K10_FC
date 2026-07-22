#include "k10_ble_hid.h"

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <BLE_HID_Gamepad.h>
#include "k10_video.h"   // display()
#include "k10_input.h"   // read() — board buttons drive the pairing UI (in the lib)

// ---- NES pad bits (mirror k10_input.cpp's NP_* so this file is self-contained) ----
#define NP_A      0x01
#define NP_B      0x02
#define NP_SELECT 0x04
#define NP_START  0x08
#define NP_UP     0x10
#define NP_DOWN   0x20
#define NP_LEFT   0x40
#define NP_RIGHT  0x80

namespace k10blehid {

// Owns the BLE_HID_Host + scan/pick/connect TFT menu (libs/BLE_HID_Gamepad).
// This file keeps only the NES button mapping: onReport() turns the host's
// {buttons, hat} into a NES-pad bitmask.
static BLE_HID_Gamepad pad;

// Running NES-pad bitmask, updated by the HID notify callback (Bluedroid task)
// and read by the frame loop. A single-byte store is atomic on Xtensa, so no
// lock is needed — same pattern as k10input::g_buttons.
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
// But cheap pads vary wildly. This table is THE tuning point; watch the serial
// `[hid] btn=... held=[...]` lines (K10_BLE_HID_PRINT=1 below, plus the raw
// `[hid] rpt ...` bytes from K10_BLE_HID_DEBUG=1 in libs/BLE_HID_Host) for your
// pad's button numbers and remap here. Index i = Button (i+1); 0 = unused.
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

    // Bring up BLE + the scan/pick/connect menu, then register this file's NES
    // decoder onto the underlying host (the lib is NES-agnostic by design).
    pad.begin(k10video::display(), k10input::read);
    pad.host().onReport(onReport);
    pad.host().onDisconnect(onDisconnect);
}

uint8_t read()        { return g_pad; }
bool    isConnected() { return pad.isConnected(); }

bool connect_flow()
{
    return pad.connect_flow();
}

} // namespace k10blehid
