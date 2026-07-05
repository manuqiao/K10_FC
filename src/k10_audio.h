#pragma once
#include <stdint.h>
#include <stddef.h>

// K10 I2S audio output for the nofrendo NES core.
// The K10's speaker amp is an external I2S DAC on the GPIO expander-routed pins.
namespace k10audio {

// Reconfigure I2S_NUM_0 (pre-installed by k10.begin() for the mic) for TX output
// to the speaker, at the given sample rate (e.g. 32000). Enables the amp.
void init(uint32_t sample_rate);

// Submit `count` mono int16 samples (one NES frame's worth) to the speaker.
// Pushes into an internal stream buffer that a dedicated core-0 audio task
// drains into I2S at the hardware rate. Blocks only when the stream is full
// (emulator running ahead of realtime), which paces emulation to real time
// while keeping the DAC fed through per-frame jitter.
void submit(const int16_t *mono, size_t count);

} // namespace k10audio
