#include "k10_input.h"

#include <Arduino.h>
#include "freertos/task.h"
#include "unihiker_k10.h"   // k10 (accelerometer, gestures)
#include "initBoard.h"      // digital_read(), eP5_KeyA / eP11_KeyB

// ---- NES pad bits (mirror nofrendo nes/input.h to keep this self-contained) ----
#define NP_A      0x01
#define NP_B      0x02
#define NP_SELECT 0x04
#define NP_START  0x08
#define NP_UP     0x10
#define NP_DOWN   0x20
#define NP_LEFT   0x40
#define NP_RIGHT  0x80

// The UNIHIKER_K10 object lives in main.cpp.
extern UNIHIKER_K10 k10;

// ============================ tunable controls ============================
// Tilt deadzone in accelerometer LSB. Bump up if the player drifts on its own;
// bump down if tilt feels sluggish. ~40-100 is a reasonable range.
static const int TILT_DZ = 70;

// Which accelerometer member feeds left/right vs up/down, and its sign.
// User-verified on HW (2026-07-04) by watching the [imu] accX/accY debug line
// while tilting, with the display at setRotation(3)):
//   accY > 0 -> RIGHT,  accY < 0 -> LEFT
//   accX > 0 -> UP,     accX < 0 -> DOWN
// If a direction is wrong after reorienting the board, flip the relevant sign
// or swap AXIS_LR / AXIS_UD here.
#define AXIS_LR   accY
#define LR_SIGN   (+1)
#define AXIS_UD   accX
#define UD_SIGN   (-1)

// --- Light sensor -> START -------------------------------------------------
// Cover the light sensor (ambient reading drops below ALS_START_THRESHOLD) to
// begin / pause. Level-triggered: START stays asserted while covered, so one
// cover gesture reads as one press. Tune from the [als] debug line (main.cpp
// prints the ambient reading each second).
static const uint16_t ALS_START_THRESHOLD = 20;

// --- A+B held together -> SELECT -------------------------------------------
// Holding both buttons ~BOTH_HOLD_MS reaches the rarely-needed SELECT. The hold
// gate avoids firing during normal jump+shoot play. Set 0 for instant, or raise
// if it misfires. (START used to live here; it moved to the light sensor.)
static const unsigned long BOTH_HOLD_MS = 600;

// Background poll interval. Buttons live on the I2C GPIO expander and each
// digital_read() blocks ~11 ms (the closed-source driver has a built-in
// debounce delay, which yields the CPU), so one full read takes ~22 ms. The
// frame loop must NOT pay that cost inline (it starved audio). 10 ms gives
// ~30 Hz effective button updates with bus gaps left for the gesture task.
#define INPUT_POLL_MS 10
// ===========================================================================

namespace k10input {

// NES pad byte, written by input_task and read by the frame loop. A single-byte
// store is atomic on Xtensa, so no lock is needed.
static volatile uint8_t g_buttons = 0;
static TaskHandle_t     g_task    = nullptr;

// Polls buttons (slow I2C expander reads) + the cached accelerometer/gesture and
// publishes the NES pad byte. Runs on core 0 alongside the audio task; both are
// almost always blocked on a bus wait, so they share cleanly.
static void input_task(void *)
{
    unsigned long bothStart = 0;
    for (;;)
    {
        uint8_t b = 0;

        // Raw expander level reads (active-low). Avoids k10.buttonX->isPressed(),
        // whose delay(5) debounce loop would block even longer when held.
        bool a  = (digital_read(eP5_KeyA)  == 0);
        bool bb = (digital_read(eP11_KeyB) == 0);

        // Remapped per request: board B -> NES A, board A -> NES B.
        if (bb) b |= NP_A;
        if (a)  b |= NP_B;

        // A+B held together -> SELECT (after BOTH_HOLD_MS, so jump+shoot play
        // doesn't accidentally fire it).
        if (a && bb)
        {
            if (bothStart == 0) bothStart = millis();
            else if (millis() - bothStart > BOTH_HOLD_MS) b |= NP_SELECT;
        }
        else bothStart = 0;

        // Cover the light sensor -> START (begin / pause). Level-triggered.
        if (k10.readALS() < ALS_START_THRESHOLD) b |= NP_START;

        // Tilt -> D-pad. accX/accY are cached by k10.begin()'s gesture task.
        int lr = LR_SIGN * (int)k10.AXIS_LR;
        int ud = UD_SIGN * (int)k10.AXIS_UD;
        if (lr >  TILT_DZ) b |= NP_RIGHT;
        if (lr < -TILT_DZ) b |= NP_LEFT;
        if (ud >  TILT_DZ) b |= NP_DOWN;
        if (ud < -TILT_DZ) b |= NP_UP;

        g_buttons = b;

        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
}

void init()
{
    xTaskCreatePinnedToCore(input_task, "k10input", 4096, nullptr, 4, &g_task, 0);
}

uint8_t read()
{
    return g_buttons;   // instant: the background task does the slow work
}

} // namespace k10input
