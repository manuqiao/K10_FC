#include "k10_matrix.h"

#include <Arduino.h>
#include "freertos/task.h"
#include "initBoard.h"   // ePin_t, digital_write() for the expander row pins

// ============================ tunable controls ============================
// Pause between full scans. Rows are on the I2C expander (each digital_write is
// a bus transaction), so the scan itself sets the pace; this just yields a
// little I2C time to k10input between scans.
#define MATRIX_SCAN_GAP_MS 5

// Settle time after switching the active row before reading the columns.
// digital_write returns after the row is actually driven, so 100us is plenty
// for the passive keypad + the native column input.
#define ROW_SETTLE_US 100

// Per-key software debounce scans. scan_once() is authoritative (it drives rows
// one at a time, so a floating/noise blip reads back as "no key"), so 1 scan is
// enough — and every extra scan is costly because the expander writes are slow.
#define DEBOUNCE_COUNT 1

// Verbose raw-state probe every MATRIX_DEBUG_MS while diagnosing the keypad.
// Prints the per-row scan result plus an "all rows LOW" probe so one test run
// shows whether the expander rows actually drive and whether P0/P1 columns
// respond. Set 0 to silence once the keypad reads cleanly.
#define MATRIX_DEBUG 0
#define MATRIX_DEBUG_MS 500
// ===========================================================================

namespace k10matrix {

// Key bits — deliberately laid out to match the NES pad (nofrendo's NES_PAD_* /
// k10input's NP_*), so k10matrix::read() feeds the menu and the NES core
// directly with no translation. kLabel[] below is indexed by these bit positions.
#define KM_A       0x01
#define KM_B       0x02
#define KM_SELECT  0x04
#define KM_START   0x08
#define KM_UP      0x10
#define KM_DOWN    0x20
#define KM_LEFT    0x40
#define KM_RIGHT   0x80

// ---- pin tables -----------------------------------------------------------
// Rows = OUTPUTS on the I2C expander. Driven LOW one at a time, others HIGH.
// Outputs actively drive, so the expander's lack of internal pull-ups does NOT
// matter here. P2/P3/P8/P13 reuse the wires the user already had connected.
static const ePin_t kRows[4] = { eP2, eP3, eP8, eP13 };
// Columns = INPUTS on the NATIVE pins P0/P1 with INPUT_PULLUP. P0/P1 are the
// only K10 edge pins with an internal pull-up, so NO external resistor is
// needed and the inputs never float. Active-low: a pressed key ties the column
// to the driven-low row -> reads LOW.
static const uint8_t kCols[2] = { P0, P1 };

// keymap[row][col] -> NES bit. Entries are permuted to match this keypad's
// physical wiring, so each physical button reports its silkscreened NES
// function. In row-major scan order (r0c0, r0c1, r1c0, ...) this yields:
// left, select, down, start, up, b, right, a.
// If your physical keypad's button order differs, just permute these entries —
// this table is the only thing to touch.
static const uint8_t kKeymap[4][2] = {
    { KM_LEFT,   KM_SELECT },   // row 0 (P2)
    { KM_DOWN,   KM_START  },   // row 1 (P3)
    { KM_UP,     KM_B      },   // row 2 (P8)
    { KM_RIGHT,  KM_A      },   // row 3 (P13)
};
// kLabel[bit] -> readable name, indexed by key-bit position (0..7).
static const char *kLabel[8] = {
    "A", "B", "Select", "Start", "Up", "Down", "Left", "Right"
};

static volatile uint8_t g_keys = 0;     // reported (debounced) bitmask
static TaskHandle_t     g_task = nullptr;

// Full per-row scan. Toggles only the row that changed each step (2 writes per
// row, not 4) to cut I2C traffic, and leaves ALL rows LOW on return so the cheap
// idle gate in matrix_task works: with all rows low, any pressed key pulls its
// column low with ZERO further expander writes, so idle polling is just two fast
// native reads and the expensive scan runs only while a key is held.
static uint8_t scan_once()
{
    uint8_t raw = 0;

    // Deselect ALL rows first. Between scans we leave every row LOW (so the idle
    // gate can detect any press cheaply), which means the not-yet-scanned rows
    // are still LOW at loop entry. Without this deselect, a single keypress would
    // ghost into every row whose line was still low -- e.g. one key reading as
    // the entire column (B+Select+Down+Left all in col P1).
    for (int i = 0; i < 4; i++) digital_write(kRows[i], HIGH);
    delayMicroseconds(ROW_SETTLE_US);

    int active = -1;
    for (int r = 0; r < 4; r++)
    {
        if (active >= 0) digital_write(kRows[active], HIGH);   // release previous
        digital_write(kRows[r], LOW);                          // drive current (only low row)
        active = r;
        delayMicroseconds(ROW_SETTLE_US);

        for (int c = 0; c < 2; c++)
        {
            // Active-low with column pull-up: a pressed key ties the column to
            // the driven-low row -> reads LOW (0).
            if (digitalRead(kCols[c]) == LOW)
                raw |= kKeymap[r][c];
        }
    }
    for (int i = 0; i < 4; i++) digital_write(kRows[i], LOW);  // back to idle: all low
    return raw;
}

static void matrix_task(void *)
{
    // Per-key debounce integrator: +1 on each "pressed" read, -1 on "released",
    // clamped to [0..255]. Reported state flips to pressed at >= DEBOUNCE_COUNT
    // and back to released once it drains to 0.
    uint8_t integ[8] = {0};
    uint8_t reported  = 0;

    Serial.println("[matrix] task started: rows P2/P3/P8/P13, cols P0/P1 (pullup)");

    // One-shot probe: print how expensive one per-row scan is, so the expander
    // write cost is visible without ongoing log spam.
    {
        uint32_t t = micros();
        scan_once();
        Serial.printf("[matrix] scan_once=%lu us\n", (unsigned long)(micros() - t));
    }

#if MATRIX_DEBUG
    uint32_t lastDbg = 0;
#endif
    for (;;)
    {
        // Cheap idle gate: between scans all rows are LOW, so an unpressed keypad
        // leaves both pulled-up columns HIGH. Skip the ~9-write per-row scan until
        // a column actually reads LOW -> idle is two fast native reads, and the
        // scan only runs while a key is held.
        uint8_t raw;
        if (digitalRead(kCols[0]) == LOW || digitalRead(kCols[1]) == LOW)
            raw = scan_once();
        else
            raw = 0;

        for (int bit = 0; bit < 8; bit++)
        {
            uint8_t mask = 1u << bit;
            if (raw & mask) { if (integ[bit] < 255) integ[bit]++; }
            else            { if (integ[bit] > 0)   integ[bit]--; }

            bool was = (reported & mask);
            bool now = was ? (integ[bit] > 0)
                           : (integ[bit] >= DEBOUNCE_COUNT);

            if (now != was)
            {
                if (now) reported |= mask;
                else     reported &= ~mask;
                Serial.printf("[matrix] %s: %s\n",
                              now ? "pressed " : "released",
                              kLabel[bit]);
            }
        }

        g_keys = reported;

#if MATRIX_DEBUG
        // Once per interval, print a raw probe to localize faults:
        //   idle  : all rows HIGH -> P0/P1 must read 1 (pull-up alive, pin free)
        //   allLow: all rows LOW  -> pressing ANY key pulls its column to 0
        //   raw   : the per-row scan bitmask (what the keypad logic actually sees)
        if (millis() - lastDbg >= MATRIX_DEBUG_MS)
        {
            lastDbg = millis();
            for (int i = 0; i < 4; i++) digital_write(kRows[i], HIGH);
            delayMicroseconds(ROW_SETTLE_US);
            int idle0 = digitalRead(kCols[0]), idle1 = digitalRead(kCols[1]);
            for (int i = 0; i < 4; i++) digital_write(kRows[i], LOW);
            delayMicroseconds(ROW_SETTLE_US);
            int low0 = digitalRead(kCols[0]), low1 = digitalRead(kCols[1]);
            Serial.printf("[matrix] dbg raw=0x%02x  idle P0=%d P1=%d | allLow P0=%d P1=%d\n",
                          raw, idle0, idle1, low0, low1);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(MATRIX_SCAN_GAP_MS));
    }
}

void init()
{
    // Rows: expander outputs, all driven LOW at idle so the cheap idle gate in
    // matrix_task works (a pressed key then pulls its column LOW with no extra
    // writes). digital_write also sets the direction to output.
    for (int r = 0; r < 4; r++)
        digital_write(kRows[r], LOW);
    // Columns: native P0/P1 with the ESP32 internal pull-up. This is the whole
    // point of using these two pins — no external resistor, no floating.
    for (int c = 0; c < 2; c++)
        pinMode(kCols[c], INPUT_PULLUP);

    if (g_task) return;   // idempotent
    xTaskCreatePinnedToCore(matrix_task, "k10matrix", 4096, nullptr, 3, &g_task, 0);
}

uint8_t read() { return g_keys; }   // NES-pad bitmask (NP_* layout) for menu + NES core

} // namespace k10matrix
