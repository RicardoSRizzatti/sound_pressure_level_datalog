/***************************************************************************//**
 * @file
 * @brief PDM microphone acquisition (sl_mic streaming, ping-pong buffers).
 *
 * Blocks of MIC_PDM_BLOCK_FRAMES mono samples arrive from the LDMA in
 * interrupt context; mic_pdm_process() hands finished blocks to the DSP from
 * the main loop and kicks the watchdog audio health flag.
 ******************************************************************************/
#ifndef MIC_PDM_H
#define MIC_PDM_H

#include <stdbool.h>
#include <stdint.h>

/// 8 ms blocks at 48 kHz.
#define MIC_PDM_BLOCK_FRAMES 384u

/// Power the microphones, start streaming. Returns false on driver error.
bool mic_pdm_start(void);

/// Stop streaming and power the microphones down.
void mic_pdm_stop(void);

/// Call from the main loop: processes any block(s) ready since last call.
void mic_pdm_process(void);

/// Blocks dropped because the main loop lagged behind the LDMA (diagnostics).
uint32_t mic_pdm_overrun_count(void);

/// Diagnostics: total blocks processed and raw stats of the last block.
uint32_t mic_pdm_blocks_total(void);
int16_t mic_pdm_last_peak(uint8_t channel);
int32_t mic_pdm_last_mean(uint8_t channel);

/// Test-tone mode: replaces the microphone samples with a synthetic sine of
/// @p freq_hz at @p amp_cdbfs (centi-dBFS, e.g. -2600 = -26 dBFS). Block
/// timing still comes from the real PDM stream. freq_hz = 0 disables.
/// Used to validate the DSP chain on boards without microphones (BRD4184A).
void mic_pdm_set_test_tone(uint32_t freq_hz, int32_t amp_cdbfs);

#endif // MIC_PDM_H
