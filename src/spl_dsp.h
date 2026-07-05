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

/// Sample rate the A-weighting coefficients were designed for.
#define SPL_DSP_SAMPLE_RATE_HZ  32000u

/// Default dBFS -> dB SPL calibration offset for the Knowles SPK0641HT4H-1.
/// Sensitivity: -26 dBFS (AES17, full-scale sine = 0 dBFS RMS) at 94 dB SPL,
/// 1 kHz. Mean-square of a full-scale sine is 0.5 (-3.01 dB), therefore:
/// SPL = 10*log10(mean_square) + 94 + 26 + 3.01 = 10*log10(ms) + 123.0
#define SPL_DSP_DEFAULT_CAL_OFFSET_DB  123.0f

/// Metrics of one closed measurement interval.
typedef struct {
  float laeq_db;      ///< Equivalent continuous level, dB(A)
  float lafmax_db;    ///< Maximum Fast-weighted level, dB(A)
  uint32_t n_samples; ///< Samples integrated in the interval
} spl_result_t;

/// Reset all filter states and accumulators.
void spl_dsp_init(void);

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
