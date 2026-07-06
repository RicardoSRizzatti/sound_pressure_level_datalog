/***************************************************************************//**
 * @file
 * @brief Sound pressure level DSP: A-weighting, Fast detector, LAeq/LAFmax.
 *
 * Feed raw microphone blocks with spl_dsp_process_block(); close a
 * measurement interval with spl_dsp_close_interval() to obtain the
 * A-weighted metrics accumulated since the previous close.
 ******************************************************************************/
#ifndef SPL_DSP_H
#define SPL_DSP_H

#include <stdbool.h>
#include <stdint.h>

/// Sample rate the filters were designed for. 48 kHz divides the 38.4 MHz
/// PDM branch clock exactly (DSR 50, prescaler 16 -> mic clock 2.400 MHz);
/// 32 kHz with DSR 64 did NOT (integer prescaler made the real rate 33.3 kHz).
#define SPL_DSP_SAMPLE_RATE_HZ  48000u

/// Default dBFS -> dB SPL calibration offset for the Knowles SPK0641HT4H-1.
/// Sensitivity: -26 dBFS (AES17, full-scale sine = 0 dBFS RMS) at 94 dB SPL,
/// 1 kHz. Mean-square of a full-scale sine is 0.5 (-3.01 dB), therefore:
/// SPL = 10*log10(mean_square) + 94 + 26 + 3.01 = 10*log10(ms) + 123.0
#define SPL_DSP_DEFAULT_CAL_OFFSET_DB  123.0f

/// Metrics of one closed measurement interval. The extended block follows
/// the B&K 2245 parameter set (IEC 61672): Slow detector, minima, C-weighted
/// peak, sound exposure level and LAF percentiles.
typedef struct {
  float laeq_db;      ///< Equivalent continuous level, dB(A)
  float lafmax_db;    ///< Maximum Fast-weighted level, dB(A)
  uint32_t n_samples; ///< Samples integrated in the interval

  bool extended;      ///< Extended metrics below are valid
  float lafmin_db;    ///< Minimum Fast level, dB(A)
  float lasmax_db;    ///< Maximum Slow (tau = 1 s) level, dB(A)
  float lasmin_db;    ///< Minimum Slow level, dB(A)
  float lcpeak_db;    ///< C-weighted peak level, dB(C)
  float lae_db;       ///< Sound exposure level (LAeq + 10 log10 T), dB(A)
  float l10_db;       ///< Level exceeded 10% of the time (LAF), dB(A)
  float l50_db;       ///< Level exceeded 50% of the time, dB(A)
  float l90_db;       ///< Level exceeded 90% of the time, dB(A)
} spl_result_t;

/// Reset all filter states and accumulators.
void spl_dsp_init(void);

/// Enable/disable the extended metric set (SPL_METRIC_EXTENDED config bit).
void spl_dsp_set_extended(bool enabled);

/// Set the dBFS -> dB SPL calibration offset (persisted elsewhere).
void spl_dsp_set_cal_offset(float offset_db);
float spl_dsp_get_cal_offset(void);

/// Process one block of mono 16-bit samples (call from main context).
void spl_dsp_process_block(const int16_t *samples, uint32_t n);

/// Close the current interval: fill @p out and restart accumulation.
/// Returns false if no samples were accumulated (out is not written).
bool spl_dsp_close_interval(spl_result_t *out);

/// Current Fast-weighted level in dB(A) (for live display/debug).
float spl_dsp_current_laf_db(void);

#endif // SPL_DSP_H
