#include "k10_audio.h"

#include <Arduino.h>
#include "driver/i2s.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "initBoard.h"   // eAmp_Gain, digital_write()

// K10 I2S speaker pinout (see unihiker_k10.h: IIS_BLCK/LRCK/DOUT/MCLK).
#define PIN_BCLK  0
#define PIN_WS    38
#define PIN_DOUT  45
#define PIN_MCLK  3

namespace k10audio {

// One NES frame at 32 kHz / 60 Hz is ~533 mono samples. The stream buffer holds
// ~7-8 frames of slack so the audio task can keep the DAC fed even when the
// emulator has a momentarily heavy frame or a blocking button read.
#define FRAME_STAGING_SAMPLES 1024   // max samples pulled per task iteration
#define AUDIO_STREAM_BYTES    8192   // ~7-8 frames of int16 slack
#define AUDIO_TASK_STACK      2048   // words

// Software output volume as a percent (0..100), applied in the drain task when
// the mono sample is fanned out to stereo. The K10 amp gain pin (eAmp_Gain) is
// only high/low, so fine volume control is done here by scaling the samples
// (sign-preserving). Set to 100 for full volume.
#define AUDIO_VOLUME_PCT      5

static StreamBufferHandle_t g_stream = nullptr;
static TaskHandle_t         g_task   = nullptr;

// Drain the stream buffer into I2S at the hardware sample rate. Pinned to core 0
// so it runs truly parallel to nes_emulate on core 1 (Arduino loop). Per-frame
// jitter is absorbed by the stream + DMA buffers instead of reaching the DAC,
// which is what removes the clicks/stutter.
static void audio_task(void *)
{
    static int16_t  mono[FRAME_STAGING_SAMPLES];
    static uint16_t stereo[FRAME_STAGING_SAMPLES * 2];

    for (;;)
    {
        size_t got = xStreamBufferReceive(g_stream, mono, sizeof(mono), portMAX_DELAY);
        size_t nsamp = got / 2;
        for (size_t j = 0; j < nsamp; j++)
        {
            int16_t s = mono[j];                       // sign-preserving
            s = (int16_t)((int32_t)s * AUDIO_VOLUME_PCT / 100);
            uint16_t u = (uint16_t)s;
            stereo[j * 2]     = u;                     // L
            stereo[j * 2 + 1] = u;                     // R
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, (const void *)stereo, nsamp * 4, &written, portMAX_DELAY);
    }
}

void init(uint32_t sample_rate)
{
    // k10.begin() installed I2S_NUM_0 for the microphones. Take it over for TX.
    i2s_driver_uninstall(I2S_NUM_0);

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = sample_rate;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;          // stereo frames
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;                 // was 6 -> ~64 ms of HW slack
    cfg.dma_buf_len = 256;
    cfg.use_apll = true;   // accurate clock for audio rates

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK)
    {
        Serial.println("[audio] i2s_driver_install failed");
        return;
    }

    i2s_pin_config_t pins = {};
    pins.mck_io_num = PIN_MCLK;
    pins.bck_io_num = PIN_BCLK;
    pins.ws_io_num = PIN_WS;
    pins.data_out_num = PIN_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);

    digital_write(eAmp_Gain, 1);   // enable / raise amp gain

    // Single-producer (loop, core 1) / single-consumer (audio_task, core 0)
    // stream. Trigger of 256 bytes wakes the task ~once per frame.
    g_stream = xStreamBufferCreate(AUDIO_STREAM_BYTES, 256);
    if (!g_stream)
    {
        Serial.println("[audio] stream buffer create failed");
        return;
    }

    xTaskCreatePinnedToCore(audio_task, "k10audio", AUDIO_TASK_STACK,
                            nullptr, 5, &g_task, /*core*/ 0);
    if (!g_task)
        Serial.println("[audio] task create failed");
}

void submit(const int16_t *mono, size_t count)
{
    if (!g_stream) return;
    // Blocks only when the stream is full, i.e. when the emulator is running
    // ahead of realtime. That backpressure paces the frame loop to the audio
    // clock; transient jitter is absorbed downstream by the DMA buffers.
    xStreamBufferSend(g_stream, mono, count * 2, portMAX_DELAY);
}

} // namespace k10audio
