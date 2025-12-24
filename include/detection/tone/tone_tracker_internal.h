/**
 * @file tone_tracker_internal.h
 * @brief Internal structures and declarations for tone tracker
 */

#ifndef TONE_TRACKER_INTERNAL_H
#define TONE_TRACKER_INTERNAL_H

#include "tone_tracker.h"
#include "fft_processor.h"
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/*============================================================================
 * Configuration Constants
 *============================================================================*/

#define SEARCH_BINS         10          /* ±10 bins = ±29 Hz search window */
#define MIN_SNR_DB          10.0f       /* Minimum SNR for valid measurement */
#define NOISE_BINS          20          /* Bins to average for noise floor */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State Structure
 *============================================================================*/

struct tone_tracker {
    float nominal_hz;           /* 500 or 600 */

    /* Sample buffer */
    float *buffer_i;
    float *buffer_q;
    int buffer_idx;
    int samples_collected;

    /* FFT */
    fft_processor_t *fft;
    float *magnitudes;

    /* Results */
    float measured_hz;
    float offset_hz;
    float offset_ppm;
    float snr_db;
    float noise_floor_linear;   /* Linear noise floor for marker baseline */
    bool valid;

    /* Logging */
    FILE *csv_file;
    uint64_t frame_count;
    time_t start_time;
};

/*============================================================================
 * FFT Helper Functions
 *============================================================================*/

/**
 * Generate Blackman-Harris window coefficients
 * @param window Output array for window coefficients
 * @param size Window size
 */
void tone_generate_blackman_harris(float *window, int size);

/**
 * Find peak bin using parabolic interpolation for sub-bin resolution
 * @param mag Magnitude array
 * @param peak_bin Integer peak bin index
 * @param fft_size FFT size
 * @return Fractional peak bin location
 */
float tone_parabolic_peak(float *mag, int peak_bin, int fft_size);

/**
 * Find peak bin in specified range
 * @param mag Magnitude array
 * @param start Start bin (inclusive)
 * @param end End bin (inclusive)
 * @param fft_size FFT size
 * @return Peak bin index
 */
int tone_find_peak_bin(float *mag, int start, int end, int fft_size);

/**
 * Estimate noise floor excluding signal region
 * @param mag Magnitude array
 * @param fft_size FFT size
 * @param exclude_bin Center bin to exclude
 * @param exclude_range Range around center to exclude
 * @return Estimated noise floor (linear)
 */
float tone_estimate_noise_floor(float *mag, int fft_size, int exclude_bin, int exclude_range);

/*============================================================================
 * Measurement Functions
 *============================================================================*/

/**
 * Measure tone frequency using FFT analysis
 * Handles both DC (0 Hz) and normal tone (500/600 Hz) cases
 * Updates tracker state with measured frequency, offset, SNR
 * @param tt Tone tracker instance
 */
void tone_measure_frequency(tone_tracker_t *tt);

/**
 * Log measurement to CSV file (if enabled)
 * @param tt Tone tracker instance
 */
void tone_log_measurement(tone_tracker_t *tt);

#endif /* TONE_TRACKER_INTERNAL_H */
