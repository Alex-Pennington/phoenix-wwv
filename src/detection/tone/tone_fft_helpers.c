/**
 * @file tone_fft_helpers.c
 * @brief FFT helper functions for tone tracking
 *
 * Provides peak finding, interpolation, and noise estimation
 * for accurate frequency measurement.
 */

#include "tone_tracker_internal.h"
#include <math.h>
#include <stdlib.h>

/*============================================================================
 * Window Function
 *============================================================================*/

void tone_generate_blackman_harris(float *window, int size) {
    const float a0 = 0.35875f;
    const float a1 = 0.48829f;
    const float a2 = 0.14128f;
    const float a3 = 0.01168f;

    for (int i = 0; i < size; i++) {
        float n = (float)i / (float)(size - 1);
        window[i] = a0
                  - a1 * cosf(2.0f * M_PI * n)
                  + a2 * cosf(4.0f * M_PI * n)
                  - a3 * cosf(6.0f * M_PI * n);
    }
}

/*============================================================================
 * Parabolic Interpolation
 *============================================================================*/

float tone_parabolic_peak(float *mag, int peak_bin, int fft_size) {
    if (peak_bin <= 0 || peak_bin >= fft_size - 1)
        return (float)peak_bin;

    float alpha = mag[peak_bin - 1];
    float beta  = mag[peak_bin];
    float gamma = mag[peak_bin + 1];

    float denom = alpha - 2.0f * beta + gamma;
    if (fabsf(denom) < 1e-10f)
        return (float)peak_bin;

    float p = 0.5f * (alpha - gamma) / denom;
    return (float)peak_bin + p;
}

/*============================================================================
 * Find Peak in Range
 *============================================================================*/

int tone_find_peak_bin(float *mag, int start, int end, int fft_size) {
    if (start < 0) start = 0;
    if (end >= fft_size) end = fft_size - 1;

    int peak_bin = start;
    float peak_val = mag[start];

    for (int i = start + 1; i <= end; i++) {
        if (mag[i] > peak_val) {
            peak_val = mag[i];
            peak_bin = i;
        }
    }

    return peak_bin;
}

/*============================================================================
 * Estimate Noise Floor
 *============================================================================*/

float tone_estimate_noise_floor(float *mag, int fft_size, int exclude_bin, int exclude_range) {
    float sum = 0.0f;
    int count = 0;

    /* Sample bins away from the tone */
    for (int i = 50; i < 150; i++) {
        if (abs(i - exclude_bin) > exclude_range) {
            sum += mag[i];
            count++;
        }
    }

    /* Also check negative frequency region */
    int neg_exclude = fft_size - exclude_bin;
    for (int i = fft_size - 150; i < fft_size - 50; i++) {
        if (abs(i - neg_exclude) > exclude_range) {
            sum += mag[i];
            count++;
        }
    }

    return (count > 0) ? (sum / count) : 1e-10f;
}
