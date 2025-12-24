/**
 * @file fft_processor.h
 * @brief Unified FFT processing interface for WWV detectors
 *
 * Eliminates duplicated FFT setup and energy extraction code across
 * tick_detector, marker_detector, bcd_time_detector, bcd_freq_detector,
 * and tone_tracker.
 *
 * Provides:
 * - FFT configuration and resource management
 * - Windowed I/Q sample processing
 * - Frequency bucket energy extraction
 */

#ifndef FFT_PROCESSOR_H
#define FFT_PROCESSOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque FFT processor handle
 */
typedef struct fft_processor fft_processor_t;

/**
 * Create FFT processor
 *
 * @param fft_size FFT size (must be power of 2)
 * @param sample_rate Sample rate in Hz
 * @return FFT processor handle, or NULL on allocation failure
 */
fft_processor_t *fft_processor_create(int fft_size, float sample_rate);

/**
 * Destroy FFT processor and free resources
 *
 * @param fft Processor to destroy (NULL safe)
 */
void fft_processor_destroy(fft_processor_t *fft);

/**
 * Process windowed I/Q samples and compute FFT
 *
 * Applies Hann window and runs FFT on provided samples.
 *
 * @param fft Processor handle
 * @param i_samples I (in-phase) samples [fft_size]
 * @param q_samples Q (quadrature) samples [fft_size]
 * @return true on success, false on invalid input
 */
bool fft_processor_process(fft_processor_t *fft, const float *i_samples, const float *q_samples);

/**
 * Get energy in frequency bucket
 *
 * Computes magnitude sum across positive and negative frequency bins
 * centered on target_freq ± bandwidth/2.
 *
 * @param fft Processor handle
 * @param target_freq Target frequency in Hz
 * @param bandwidth Bandwidth in Hz
 * @return Total energy (magnitude sum), normalized by FFT size
 */
float fft_processor_get_bucket_energy(fft_processor_t *fft, float target_freq, float bandwidth);

/**
 * Get Hz per FFT bin
 *
 * @param fft Processor handle
 * @return Frequency resolution in Hz/bin
 */
float fft_processor_get_hz_per_bin(fft_processor_t *fft);

/**
 * Get FFT size
 *
 * @param fft Processor handle
 * @return FFT size
 */
int fft_processor_get_size(fft_processor_t *fft);

/**
 * Get direct access to FFT output for magnitude calculation
 *
 * Use only after calling fft_processor_process(). Output remains valid
 * until next fft_processor_process() call.
 *
 * @param fft Processor handle
 * @param magnitudes Output buffer [fft_size] to fill with magnitudes
 */
void fft_processor_get_magnitudes(fft_processor_t *fft, float *magnitudes);

#ifdef __cplusplus
}
#endif

#endif /* FFT_PROCESSOR_H */
