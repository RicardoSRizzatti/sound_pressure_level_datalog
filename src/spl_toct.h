/***************************************************************************//**
 * @file
 * @brief 1/3-octave spectrum analyser (multirate IIR filter bank).
 *
 * 30 bands, 20 Hz to 16 kHz, IEC 61260-style: 6th-order Butterworth
 * band-pass per band, run at one of 5 decimated rates (÷4 per stage) so
 * low bands stay numerically healthy in float32 and the CPU cost stays low.
 * Feed raw (unweighted) float blocks continuously; per measurement interval
 * it yields the equivalent level of each band in dB(Z) (no weighting).
 *
 * Filters designed/verified by tools/design_third_octave.py.
 ******************************************************************************/
#ifndef SPL_TOCT_H
#define SPL_TOCT_H

#include <stdbool.h>
#include <stdint.h>

#define SPL_TOCT_BANDS 30u

/// Reset all filter states and accumulators.
void spl_toct_init(void);

/// Enable/disable processing (driven by the SPL_METRIC_SPECTRUM config bit).
void spl_toct_set_enabled(bool enabled);
bool spl_toct_enabled(void);

/// Process one block of raw float samples in [-1, 1) at 48 kHz.
/// The input buffer is not modified.
void spl_toct_process_block(const float *samples, uint32_t n);

/// Close the interval: band levels in centi-dB(Z) (re the same calibration
/// offset as the broadband metrics). Returns false if nothing accumulated.
bool spl_toct_close_interval(int16_t bands_cdb[SPL_TOCT_BANDS]);

/// Last closed interval (for the CLI `spec` command). False if none yet.
bool spl_toct_last(int16_t bands_cdb[SPL_TOCT_BANDS]);

/// Nominal center frequency of band i, in tenths of Hz (315 = 31.5 Hz).
uint32_t spl_toct_center_dhz(uint32_t band);

#endif // SPL_TOCT_H
