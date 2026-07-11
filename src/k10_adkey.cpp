#include "k10_adkey.h"

#include <Arduino.h>
#include <Wire.h>
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

// ============================ extender (DFR1231) I2C protocol =================
// The keypad is on the IO Extender's C0. The extender chip is at I2C 0x33 on the
// SAME Wire bus k10.begin() brings up (SDA47/SCL48), so no extra pin/bus setup.
// Register map (lifted from DFRobot_UnihikerExpansion):
#define EXTENDER_ADDR     0x33
#define REG_IO_MODE_C0    0x2c   // +port: write 1 byte to set C0..C3 mode (0x00 = ADC)
#define REG_ADC_C0        0x45   // +port*3: read 3 bytes [status, hi, lo]
#define REG_RESET         0xa0   // write DATA_ENABLE to reset the extender chip
#define DATA_ENABLE       0x01   // status: a sample is ready in [hi, lo]
#define MODE_ERROR        0x02   // status: port not in ADC mode (re-set it and retry)
// =============================================================================

// ============================ tunable controls ===============================
// Which extender multi-function port the keypad is wired to (0=C0..3=C3).
#define ADKEY_PORT        0

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
// CALIBRATED on real HW (2026-07-10) by reading the [adkey] debug line while
// pressing each key. This DFR0075 variant holds its keys high (all in
// 3072..3740, no-key clamps to 4095), NOT the low 120..3041 a naive 10-bit
// scaling predicts — so the classic defaults are useless here. Each entry is the
// MIDPOINT between two adjacent keys' centers (optimal for symmetric noise);
// re-measure with ADKEY_DEBUG=1 if you swap the module.
//   s1~3072  s2~3185  s3~3320  s4~3500  s5~3740  none~4095
static const uint16_t kKeyThr[5] = { 3128, 3252, 3410, 3620, 3917 };

// index 0..4 (s1..s5) -> NES bit. Per spec:
//   s1 -> A   s2 -> Up   s3 -> Left   s4 -> Down   s5 -> Right
static const uint8_t kKey2NES[5] = { NP_A, NP_UP, NP_LEFT, NP_DOWN, NP_RIGHT };

// Consecutive-sample debounce on the decoded key *identity* (not per-bit). The
// ADC sits clean-ish between keys but can bounce right on a band edge; requiring
// the same key for DEBOUNCE_COUNT polls in a row rejects that flicker without
// per-bit contention. Poll period is ~6ms (dominated by the 3ms C0 read), so 2
// samples ~= 12ms to register a press.
#define DEBOUNCE_COUNT    2

// Hold a newly pressed direction/A key for at least MIN_PRESS_MS even after it
// releases, so a quick tap can't fall entirely between two reads of g_buttons
// (the NES frame loop reads every ~14-28ms). A long hold releases immediately
// (its deadline already elapsed). Mirrors k10_matrix's MIN_PRESS_MS rationale.
#define MIN_PRESS_MS      12

// Background poll gap (ms). The work per poll (one C0 ADC read) sets most of the
// pace; this just yields a little bus time. The expander board-button reads are
// the slow part (~11ms each), so they only run every BUTTON_DIV polls —
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

// --- I2C helpers (register access to the extender at 0x33) ------------------
static uint8_t ext_write(uint8_t reg, const uint8_t *data, uint8_t len)
{
    Wire.beginTransmission(EXTENDER_ADDR);
    Wire.write(reg);
    for (uint8_t i = 0; i < len; i++) Wire.write(data[i]);
    return Wire.endTransmission();   // 0 == success
}

static uint8_t ext_read(uint8_t reg, uint8_t *out, uint8_t len)
{
    // Match DFRobot's readReg: write the register (with STOP), then requestFrom.
    if (ext_write(reg, nullptr, 0) != 0) return 0;
    uint8_t got = Wire.requestFrom((uint8_t)EXTENDER_ADDR, (uint8_t)len);
    uint8_t i = 0;
    while (Wire.available() && i < len) out[i++] = Wire.read();
    return i;
}

// Reset the extender chip to a known state (mirrors its library's begin(): write
// DATA_ENABLE to the reset reg, then wait for it to ACK again). Bounded so a
// missing extender can't hang boot — if it never ACKs, ADC reads just stay at
// 0xFFFF and the [adkey] debug line shows adc=65535 (clearly "nothing there").
static void reset_extender()
{
    uint8_t en = DATA_ENABLE;
    ext_write(REG_RESET, &en, 1);
    for (int i = 0; i < 50; i++)   // up to ~250ms; the chip is normally back in <50ms
    {
        Wire.beginTransmission(EXTENDER_ADDR);
        if (Wire.endTransmission() == 0) break;
        delay(5);
    }
}

// Put C0 into ADC mode. Cheap; called once at task start and again if a read
// ever reports MODE_ERROR (e.g. the extender was reset).
static void set_adc_mode()
{
    uint8_t mode = 0x00;   // eADC
    for (int retry = 0; retry < 5; retry++)
    {
        if (ext_write(REG_IO_MODE_C0 + ADKEY_PORT, &mode, 1) == 0) return;
        delay(20);
    }
}

// One C0 sample. Returns 0..4095, or 0xFFFF on a hard error / not-ready timeout.
// Applies the extender's own end clamps (<40 -> 0, >3900 -> 4095) for stability.
static uint16_t read_adc()
{
    uint8_t buf[3] = {0};
    uint8_t reg = REG_ADC_C0 + ADKEY_PORT * 3;
    for (int retry = 0; retry < 5; retry++)
    {
        if (ext_read(reg, buf, 3) == 3)
        {
            if (buf[0] == DATA_ENABLE)
            {
                uint16_t v = ((uint16_t)buf[1] << 8) | buf[2];
                if (v > 3900)      v = 4095;
                else if (v < 40)   v = 0;
                return v;
            }
            if (buf[0] == MODE_ERROR) set_adc_mode();   // re-arm, then retry
        }
        delay(2);
    }
    return 0xFFFF;
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
    reset_extender();   // bring the extender chip to a known state (clears all C/S modes)
    set_adc_mode();
    Serial.printf("[adkey] task started: extender C%d @ I2C 0x%02x\n", ADKEY_PORT, EXTENDER_ADDR);

    int8_t   stable = -1;     // currently asserted key (debounced identity)
    int8_t   cand   = -1;     // candidate key seen this run
    uint8_t  candN  = 0;      // consecutive polls cand has held
    uint8_t  holdBit = 0;     // direction/A bit being held out (min-press window)
    uint32_t holdUntil = 0;   // deadline for holdBit
    uint8_t  boardBits = 0;   // sampled SELECT/START, held between samples
    uint8_t  poll = 0;
    uint32_t lastDbg = 0;
    uint16_t lastAdc = 0;     // for the debug line when a read errors out
    int8_t   lastKey = -1;

    for (;;)
    {
        uint16_t adc = read_adc();
        int8_t raw = (adc == 0xFFFF) ? stable : decode_key(adc);  // keep last on a read error
        lastAdc = adc;

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
                          (unsigned)lastAdc, (int)lastKey, (int)stable, out);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void init()
{
    // C0 is on the extender chip on the shared Wire bus (already up from
    // k10.begin()); no GPIO config. Mode is set inside the task.
    if (g_task) return;   // idempotent
    xTaskCreatePinnedToCore(adkey_task, "k10adkey", 4096, nullptr, 3, &g_task, 0);
}

uint8_t read() { return g_buttons; }

} // namespace k10adkey
