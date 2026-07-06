/***************************************************************************//**
 * @file
 * @brief PDM microphone acquisition implementation.
 *
 * The sl_mic driver ping-pongs between two DMA buffers, so the consumer has
 * one block period (10 ms) to react. To survive longer main-loop stalls
 * (CLI prints, NVM3 writes, BLE bursts) the ISR copies each finished block
 * into a small FIFO; blocks are only dropped once the FIFO itself is full
 * (MIC_PDM_QUEUE_BLOCKS x 10 ms of tolerance).
 ******************************************************************************/
#include "mic_pdm.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "app.h"
#include "app_watchdog.h"
#include "sl_board_control.h"
#include "sl_mic.h"
#include "sl_sleeptimer.h"
#include "spl_dsp.h"

/// FIFO depth: tolerates up to 40 ms of main-loop latency. (Stereo blocks
/// are twice the size, so the depth is halved to keep the BLE heap intact.)
#define MIC_PDM_QUEUE_BLOCKS 4u

/// Mono capture (left mic). Stereo doubled the FIFO RAM and its diagnostic
/// job (the BRD4184A/B pin mix-up) is done; the BLE heap needs the space.
#define MIC_PDM_CHANNELS 1u

// Ping-pong buffer: the driver fills one half while the other is copied out.
static int16_t pingpong[2 * MIC_PDM_BLOCK_FRAMES * MIC_PDM_CHANNELS];

// Block FIFO between LDMA interrupt context and the main loop.
static int16_t queue[MIC_PDM_QUEUE_BLOCKS][MIC_PDM_BLOCK_FRAMES * MIC_PDM_CHANNELS];

// Mono (channel 0) samples handed to the DSP.
static int16_t mono[MIC_PDM_BLOCK_FRAMES];

// Test-tone generator (validation on mic-less boards).
static uint32_t tone_freq_hz;
static float tone_amp;      // linear peak amplitude, 1.0 = full scale
static float tone_phase;    // radians
static volatile uint8_t q_write;   // ISR producer index
static volatile uint8_t q_read;    // main-loop consumer index
static volatile uint32_t overruns;

// Diagnostics (main-loop context), per channel.
static uint32_t blocks_total;
static int16_t last_peak[MIC_PDM_CHANNELS];
static int32_t last_mean[MIC_PDM_CHANNELS];

static bool running;

static void on_block_ready(const void *buffer, uint32_t n_frames)
{
  (void)n_frames;
  uint8_t next = (uint8_t)((q_write + 1) % MIC_PDM_QUEUE_BLOCKS);
  if (next == q_read) {
    overruns++;   // FIFO full: main loop stalled for > queue depth
  } else {
    memcpy(queue[q_write], buffer,
           MIC_PDM_BLOCK_FRAMES * MIC_PDM_CHANNELS * sizeof(int16_t));
    q_write = next;
  }
  app_proceed();
}

bool mic_pdm_start(void)
{
  if (running) {
    return true;
  }

  if (sl_board_enable_sensor(SL_BOARD_SENSOR_MICROPHONE) != SL_STATUS_OK) {
    return false;
  }
  // The microphone supply switch takes ~100 ms to settle (board regulator).
  sl_sleeptimer_delay_millisecond(100);
  if (sl_mic_init(SPL_DSP_SAMPLE_RATE_HZ, MIC_PDM_CHANNELS) != SL_STATUS_OK) {
    return false;
  }
  q_write = 0;
  q_read = 0;
  if (sl_mic_start_streaming(pingpong, MIC_PDM_BLOCK_FRAMES,
                             on_block_ready) != SL_STATUS_OK) {
    (void)sl_mic_deinit();
    return false;
  }

  running = true;
  app_watchdog_audio_watch_enable(true);
  return true;
}

void mic_pdm_stop(void)
{
  if (!running) {
    return;
  }
  app_watchdog_audio_watch_enable(false);
  (void)sl_mic_stop();
  (void)sl_mic_deinit();
  (void)sl_board_disable_sensor(SL_BOARD_SENSOR_MICROPHONE);
  running = false;
}

void mic_pdm_process(void)
{
  // Bounded drain: if the DSP can't keep up with real time, we must still
  // return to the main loop (watchdog feed, BLE); the FIFO then overflows
  // and the overrun counter exposes the overload instead of a reset.
  uint32_t budget = MIC_PDM_QUEUE_BLOCKS;
  while (q_read != q_write && budget-- > 0) {
    const int16_t *block = queue[q_read];

    for (uint32_t ch = 0; ch < MIC_PDM_CHANNELS; ch++) {
      int16_t peak = 0;
      int32_t sum = 0;
      for (uint32_t i = 0; i < MIC_PDM_BLOCK_FRAMES; i++) {
        int16_t v = block[i * MIC_PDM_CHANNELS + ch];
        int16_t a = (int16_t)(v < 0 ? -v : v);
        if (a > peak) {
          peak = a;
        }
        sum += v;
      }
      last_peak[ch] = peak;
      last_mean[ch] = sum / (int32_t)MIC_PDM_BLOCK_FRAMES;
    }
    blocks_total++;

    // SPL uses the left microphone (channel 0).
    if (tone_freq_hz != 0) {
      // Synthetic sine replaces the mic samples (DSP validation mode).
      float w = 2.0f * (float)M_PI * (float)tone_freq_hz
                / (float)SPL_DSP_SAMPLE_RATE_HZ;
      for (uint32_t i = 0; i < MIC_PDM_BLOCK_FRAMES; i++) {
        mono[i] = (int16_t)(tone_amp * 32767.0f * sinf(tone_phase));
        tone_phase += w;
        if (tone_phase > 2.0f * (float)M_PI) {
          tone_phase -= 2.0f * (float)M_PI;
        }
      }
    } else {
      for (uint32_t i = 0; i < MIC_PDM_BLOCK_FRAMES; i++) {
        mono[i] = block[i * MIC_PDM_CHANNELS];
      }
    }
    spl_dsp_process_block(mono, MIC_PDM_BLOCK_FRAMES);
    q_read = (uint8_t)((q_read + 1) % MIC_PDM_QUEUE_BLOCKS);
    app_watchdog_audio_kick();
  }
}

uint32_t mic_pdm_overrun_count(void)
{
  return overruns;
}

uint32_t mic_pdm_blocks_total(void)
{
  return blocks_total;
}

int16_t mic_pdm_last_peak(uint8_t channel)
{
  return channel < MIC_PDM_CHANNELS ? last_peak[channel] : 0;
}

int32_t mic_pdm_last_mean(uint8_t channel)
{
  return channel < MIC_PDM_CHANNELS ? last_mean[channel] : 0;
}

void mic_pdm_set_test_tone(uint32_t freq_hz, int32_t amp_cdbfs)
{
  tone_freq_hz = freq_hz;
  tone_amp = powf(10.0f, (float)amp_cdbfs / 2000.0f);
  tone_phase = 0.0f;
}
