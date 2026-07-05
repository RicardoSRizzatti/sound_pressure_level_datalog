/***************************************************************************//**
 * @file
 * @brief 1/3-octave spectrum analyser implementation (CMSIS-DSP).
 ******************************************************************************/
#include "spl_toct.h"

#include <math.h>
#include <string.h>

#include "arm_math.h"
#include "spl_dsp.h"
#include "spl_toct_coeffs.h"

/// Largest block at the full rate (8 ms at 48 kHz).
#define MAX_FRAMES 384u
/// Largest block after the first ÷4 stage (with carry margin).
#define MAX_DECIM_FRAMES (MAX_FRAMES / SPL_TOCT_DECIM_FACTOR + 2u)

/// Guard against log10(0) in silence.
#define MIN_MEAN_SQUARE 1e-14f

static bool enabled;

// Band-pass filters (3 sections each) and per-band energy accumulators.
static arm_biquad_cascade_df2T_instance_f32 band_filt[SPL_TOCT_BANDS];
static float32_t band_state[SPL_TOCT_BANDS][2 * SPL_TOCT_BAND_SECTIONS];
static float band_energy[SPL_TOCT_BANDS];

// Anti-alias decimator per stage transition (5 sections each) + phase.
static arm_biquad_cascade_df2T_instance_f32 decim_filt[SPL_TOCT_NUM_STAGES - 1];
static float32_t decim_state[SPL_TOCT_NUM_STAGES - 1][2 * SPL_TOCT_DECIM_SECTIONS];
static uint8_t decim_phase[SPL_TOCT_NUM_STAGES - 1];

// Samples accumulated per stage in the current interval.
static uint32_t stage_count[SPL_TOCT_NUM_STAGES];

// Scratch buffers: band output + the two alternating decimated streams.
static float32_t scratch[MAX_FRAMES];
static float32_t dec_a[MAX_DECIM_FRAMES];
static float32_t dec_b[MAX_DECIM_FRAMES / SPL_TOCT_DECIM_FACTOR + 2u];

static int16_t last_bands[SPL_TOCT_BANDS];
static bool has_last;

void spl_toct_init(void)
{
  // The generated header is already in CMSIS df2T layout {b0,b1,b2,-a1,-a2},
  // so the instances point straight at flash (no RAM copy; CMSIS never
  // writes through pCoeffs).
  for (uint32_t b = 0; b < SPL_TOCT_BANDS; b++) {
    arm_biquad_cascade_df2T_init_f32(
      &band_filt[b], SPL_TOCT_BAND_SECTIONS,
      (float32_t *)&spl_toct_band_sos[b * SPL_TOCT_BAND_SECTIONS][0],
      band_state[b]);
    band_energy[b] = 0.0f;
  }
  for (uint32_t d = 0; d < SPL_TOCT_NUM_STAGES - 1; d++) {
    arm_biquad_cascade_df2T_init_f32(
      &decim_filt[d], SPL_TOCT_DECIM_SECTIONS,
      (float32_t *)&spl_toct_decim_sos[d * SPL_TOCT_DECIM_SECTIONS][0],
      decim_state[d]);
    decim_phase[d] = 0;
  }
  memset(stage_count, 0, sizeof(stage_count));
  has_last = false;
}

void spl_toct_set_enabled(bool en)
{
  if (en && !enabled) {
    spl_toct_init();   // clean states when (re)starting
  }
  enabled = en;
}

bool spl_toct_enabled(void)
{
  return enabled;
}

/// Run every band of @p stage over @p in, accumulating energy.
static void run_stage_bands(uint32_t stage, const float32_t *in, uint32_t n)
{
  if (n == 0) {
    return;
  }
  stage_count[stage] += n;
  for (uint32_t b = 0; b < SPL_TOCT_BANDS; b++) {
    if (spl_toct_band_stage[b] != stage) {
      continue;
    }
    arm_biquad_cascade_df2T_f32(&band_filt[b], (float32_t *)in, scratch, n);
    float32_t e;
    arm_power_f32(scratch, n, &e);
    band_energy[b] += e;
  }
}

/// Low-pass @p in, keep every 4th sample into @p out; returns output count.
static uint32_t decimate(uint32_t d, const float32_t *in, uint32_t n, float32_t *out)
{
  arm_biquad_cascade_df2T_f32(&decim_filt[d], (float32_t *)in, scratch, n);
  uint32_t m = 0;
  uint8_t phase = decim_phase[d];
  for (uint32_t i = 0; i < n; i++) {
    if (phase == 0) {
      out[m++] = scratch[i];
    }
    phase = (uint8_t)((phase + 1) % SPL_TOCT_DECIM_FACTOR);
  }
  decim_phase[d] = phase;
  return m;
}

void spl_toct_process_block(const float *samples, uint32_t n)
{
  if (!enabled || n == 0 || n > MAX_FRAMES) {
    return;
  }

  // Stage 0 at the full rate, then walk down the decimation chain.
  run_stage_bands(0, samples, n);
  uint32_t n1 = decimate(0, samples, n, dec_a);
  run_stage_bands(1, dec_a, n1);
  uint32_t n2 = decimate(1, dec_a, n1, dec_b);
  run_stage_bands(2, dec_b, n2);
  uint32_t n3 = decimate(2, dec_b, n2, dec_a);   // reuse buffers going down
  run_stage_bands(3, dec_a, n3);
  uint32_t n4 = decimate(3, dec_a, n3, dec_b);
  run_stage_bands(4, dec_b, n4);
}

bool spl_toct_close_interval(int16_t bands_cdb[SPL_TOCT_BANDS])
{
  if (stage_count[0] == 0) {
    return false;
  }

  float cal = spl_dsp_get_cal_offset();
  for (uint32_t b = 0; b < SPL_TOCT_BANDS; b++) {
    uint32_t count = stage_count[spl_toct_band_stage[b]];
    float ms = (count > 0) ? band_energy[b] / (float)count : 0.0f;
    if (ms < MIN_MEAN_SQUARE) {
      ms = MIN_MEAN_SQUARE;
    }
    float db = 10.0f * log10f(ms) + cal;
    if (db > 320.0f) {
      db = 320.0f;
    }
    if (db < -320.0f) {
      db = -320.0f;
    }
    bands_cdb[b] = (int16_t)lrintf(db * 100.0f);
    band_energy[b] = 0.0f;
  }
  memset(stage_count, 0, sizeof(stage_count));

  memcpy(last_bands, bands_cdb, sizeof(last_bands));
  has_last = true;
  return true;
}

bool spl_toct_last(int16_t bands_cdb[SPL_TOCT_BANDS])
{
  if (!has_last) {
    return false;
  }
  memcpy(bands_cdb, last_bands, sizeof(last_bands));
  return true;
}

uint32_t spl_toct_center_dhz(uint32_t band)
{
  return (band < SPL_TOCT_BANDS) ? spl_toct_band_cfreq_dhz[band] : 0;
}
