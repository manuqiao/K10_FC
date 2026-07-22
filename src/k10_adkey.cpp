#include "k10_adkey.h"

#include <Arduino.h>
#include "freertos/task.h"
#include "initBoard.h"   // eP5_KeyA / eP11_KeyB — board buttons live on the I2C expander

// ---- NES pad bits (mirror k10_input.cpp / k10_matrix.cpp's NP_*) ----
#define NP_A      0x01
#define NP_B      0x02
#define NP_SELECT 0x04
#define NP_START  0x08
#define NP_UP     0x10
#define NP_DOWN   0x20
#define NP_LEFT   0x40
#define NP_RIGHT  0x80

// ============================ tunable controls ===============================
// ADKeyboard signal wired to the K10 board's OWN Gravity IO interface — the 3-pin
// PH2.0 full-function analog port — NOT the IO Extender's C0. That port is a
// native ESP32-S3 ADC1 pin (ADC1 is WiFi-safe; ADC2 is not): A0 = GPIO1 =
// ADC1_CH0. Switch to A1 (GPIO2 = ADC1_CH1) if you plugged into the second
// Gravity port. analogRead() on this pin is plain native ADC — no I2C involved.
#define ADKEY_PIN         A0

// 12-bit ADC (0..4095). Pinned explicitly so a framework default change can't
// silently rescale the thresholds below.
#define ADKEY_RESOLUTION  12

// How many raw samples to average per read. Averaging would tame the ESP32 ADC's
// noise and help s5 (the tightest key — see kKeyThr) — BUT back-to-back
// analogRead() calls on this ADC bias the result HIGH (~+25 counts observed),
// just enough to push s5 back above its threshold: with OS=8 s5 stopped
// registering entirely (s1-s4 have hundreds of counts of margin, so they were
// fine). So this is OFF (1) for now. Re-enable only together with inter-sample
// settling (delayMicroseconds) or esp_adc_cal. Each read is ~tens of us.
#define ADKEY_OVERSAMPLE  1

// 1 = print "[adkey] adc=.. raw=.. key=.. out=.." every ADKEY_DEBUG_MS so you can
// read off the real per-key ADC values and fill in kKeyThr below. LEAVE ON for
// the first run, press each key once, copy the numbers into kKeyThr, then set 0.
// (Same calibration story as the matrix keypad / HID gamepad modes.)
#define ADKEY_DEBUG       1
#define ADKEY_DEBUG_MS    400

// Decode table: a key is "s<k>" pressed iff the 12-bit ADC reading is below
// kKeyThr[k] (DFRobot's classic getKey — keys are a resistor ladder, so only one
// is active at a time and lower key index = lower voltage). Order MUST match
// kKey2NES: index 0 = s1 ... index 4 = s5.
//
// CALIBRATED on real HW (2026-07-12, Gravity A0, native ESP32 ADC, ADKEY_DEBUG=1)
// by pressing each key in turn (a/up/down/left/right), with adc=4095 separating
// the segments. This DFR0075 variant holds its keys HIGH (resistor ladder to
// VCC), so a LOWER adc = a LOWER key index, and no-key clamps to full-scale 4095
// — NOT the low 30..760 the classic 10-bit defaults predict. Per-key centers:
//   s1(a)~2997  s2(up)~3138  s3(left)~3303  s4(down)~3551  s5(right)~3905  none~4095
// Each entry is the MIDPOINT of the two adjacent centers, so the margin to either
// neighbor is symmetric (~56-177 counts; ESP32 ADC noise is ~+-10, comfortable).
// The clean 4095 no-key is what gives s5 ~190 counts of headroom — an earlier
// capture saw no-key clamp at ~3935 instead; if YOURS ever drops below ~4000 at
// idle (Right flickers with nothing pressed), the pin's no-key voltage has fallen,
// so lower kKeyThr[4] toward it. Re-run ADKEY_DEBUG=1 and recompute the midpoints
// if you swap the module or move to the A1 port.
static const uint16_t kKeyThr[5] = { 3068, 3220, 3427, 3728, 4000 };

// index 0..4 (s1..s5) -> NES bit. Per spec:
//   s1 -> A   s2 -> Up   s3 -> Left   s4 -> Down   s5 -> Right
static const uint8_t kKey2NES[5] = { NP_A, NP_UP, NP_LEFT, NP_DOWN, NP_RIGHT };

// Consecutive-sample debounce on the decoded key *identity* (not per-bit). The
// ADC sits clean-ish between keys but can bounce right on a band edge; requiring
// the same key for DEBOUNCE_COUNT polls in a row rejects that flicker without
// per-bit contention. analogRead() on a native ADC1 pin is fast (~tens of us),
// so the poll period is essentially POLL_MS — 2 samples ~= 6ms to register a press.
#define DEBOUNCE_COUNT    2

// Hold a newly pressed direction/A key for at least MIN_PRESS_MS even after it
// releases, so a quick tap can't fall entirely between two reads of g_buttons
// (the NES frame loop reads every ~14-28ms). A long hold releases immediately
// (its deadline already elapsed). Mirrors k10_matrix's MIN_PRESS_MS rationale.
#define MIN_PRESS_MS      12

// Background poll gap (ms). The native ADC read itself is fast; this just paces
// the task. The board-button reads for SELECT/START still go through the I2C
// expander (~11ms each — the slow part), so they only run every BUTTON_DIV polls:
// SELECT/START are menu/pause buttons, not twitch inputs, so ~22Hz sampling is
// plenty and keeps the D-pad/A loop fast.
#define POLL_MS           3
#define BUTTON_DIV        4
// =============================================================================

namespace k10adkey {

// NES pad byte, written by adkey_task and read by the frame loop. Single-byte
// store is atomic on Xtensa, so no lock.
static volatile uint8_t g_buttons = 0;
static TaskHandle_t     g_task    = nullptr;

// One native ADC sample (0..4095), averaged over ADKEY_OVERSAMPLE reads to tame
// the ESP32 ADC's noise (it's what keeps s5 reliable — see kKeyThr). analogRead()
// reconfigures the pad for analog input each call, so no pinMode setup is needed.
static uint16_t read_adc()
{
    uint32_t sum = 0;
    for (int i = 0; i < ADKEY_OVERSAMPLE; i++) sum += analogRead(ADKEY_PIN);
    return (uint16_t)(sum / ADKEY_OVERSAMPLE);
}

// Decode the 12-bit reading to a key index 0..4 (s1..s5), or -1 (no key).
static int8_t decode_key(uint16_t adc)
{
    for (int k = 0; k < 5; k++)
        if (adc < kKeyThr[k]) return k;
    return -1;   // reading above every threshold -> no key (≈ full-scale VCC)
}

static void adkey_task(void *)
{
    analogReadResolution(ADKEY_RESOLUTION);
    Serial.printf("[adkey] task started: native ADC on ADKEY_PIN (GPIO%d)\n", (int)ADKEY_PIN);

    int8_t   stable = -1;     // currently asserted key (debounced identity)
    int8_t   cand   = -1;     // candidate key seen this run
    uint8_t  candN  = 0;      // consecutive polls cand has held
    uint8_t  holdBit = 0;     // direction/A bit being held out (min-press window)
    uint32_t holdUntil = 0;   // deadline for holdBit
    uint8_t  boardBits = 0;   // sampled SELECT/START, held between samples
    uint8_t  poll = 0;
    uint32_t lastDbg = 0;
    uint16_t lastAdc = 0;
    int8_t   lastRaw = -1;   // instantaneous decode_key() of the latest sample
    int8_t   lastKey = -1;

    for (;;)
    {
        uint16_t adc = read_adc();
        int8_t raw = decode_key(adc);   // native read never errors out, so no sentinel
        lastAdc = adc;
        lastRaw = raw;

        // Consecutive-sample debounce on the key identity.
        if (raw == cand) { if (candN < 255) candN++; }
        else             { cand = raw; candN = 1; }
        int8_t key = (candN >= DEBOUNCE_COUNT) ? cand : stable;
        stable = key;
        uint8_t keyBit = (key >= 0) ? kKey2NES[key] : 0;
        lastKey = key;

        // Min-press hold for the direction/A bit.
        uint32_t now = millis();
        if (keyBit != 0)
        {
            if (keyBit != holdBit) { holdBit = keyBit; holdUntil = now + MIN_PRESS_MS; }
        }
        else if (holdBit != 0 && (int32_t)(now - holdUntil) >= 0)
            holdBit = 0;

        uint8_t out = holdBit;

        // Board buttons -> SELECT (A) / START (B). Slow expander reads, so only
        // every BUTTON_DIV polls; the last sample is held until the next.
        if (++poll >= BUTTON_DIV)
        {
            poll = 0;
            boardBits = 0;
            if (digital_read(eP5_KeyA)  == 0) boardBits |= NP_SELECT;   // board A -> Select
            if (digital_read(eP11_KeyB) == 0) boardBits |= NP_START;    // board B -> Start
        }
        out |= boardBits;

        g_buttons = out;

#if ADKEY_DEBUG
        if (millis() - lastDbg >= ADKEY_DEBUG_MS)
        {
            lastDbg = millis();
            Serial.printf("[adkey] adc=%-4u rawkey=%d stable=%d out=0x%02x\n",
                          (unsigned)lastAdc, (int)lastRaw, (int)stable, out);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void init()
{
    // ADKEY_PIN is a native ESP32 ADC1 pin — analogRead() configures the pad, so
    // no GPIO setup is needed. The board buttons stay on the I2C expander (read
    // inside the task for SELECT/START), which is why main() still suspends the
    // board poller and matrix scan: to keep the shared Wire bus uncontended
    // (Wire isn't thread-safe between tasks).
    if (g_task) return;   // idempotent
    xTaskCreatePinnedToCore(adkey_task, "k10adkey", 4096, nullptr, 3, &g_task, 0);
}

uint8_t read() { return g_buttons; }

} // namespace k10adkey
