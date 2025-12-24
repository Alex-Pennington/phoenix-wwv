/**
 * @file tone_measurement.c
 * @brief Tone frequency measurement and logging
 *
 * Core frequency measurement using dual-sideband FFT analysis
 * with parabolic interpolation for sub-bin resolution.
 */

#include "tone_tracker_internal.h"
#include "version.h"
#include <math.h>
#include <string.h>
#include <time.h>

/*============================================================================
 * Core Measurement
 *============================================================================*/

void tone_measure_frequency(tone_tracker_t *tt) {
    /* Rearrange circular buffer into linear arrays for FFT */
    float temp_i[TONE_FFT_SIZE];
    float temp_q[TONE_FFT_SIZE];
    for (int i = 0; i < TONE_FFT_SIZE; i++) {
        int idx = (tt->buffer_idx + i) % TONE_FFT_SIZE;
        temp_i[i] = tt->buffer_i[idx];
        temp_q[i] = tt->buffer_q[idx];
    }

    /* Run FFT */
    fft_processor_process(tt->fft, temp_i, temp_q);

    /* Get magnitudes */
    fft_processor_get_magnitudes(tt->fft, tt->magnitudes);

    /* Special case for DC/carrier (0 Hz) */
    if (tt->nominal_hz < 1.0f) {
        /* Find peak near DC (bin 0) - search both positive and negative freqs */
        int peak_bin = 0;
        float peak_mag = tt->magnitudes[0];

        /* Search positive frequencies (bins 1 to SEARCH_BINS) */
        for (int i = 1; i <= SEARCH_BINS && i < TONE_FFT_SIZE/2; i++) {
            if (tt->magnitudes[i] > peak_mag) {
                peak_mag = tt->magnitudes[i];
                peak_bin = i;
            }
        }

        /* Search negative frequencies (bins FFT_SIZE-1 down to FFT_SIZE-SEARCH_BINS) */
        for (int i = TONE_FFT_SIZE - 1; i >= TONE_FFT_SIZE - SEARCH_BINS; i--) {
            if (tt->magnitudes[i] > peak_mag) {
                peak_mag = tt->magnitudes[i];
                peak_bin = i;
            }
        }

        /* Convert bin to Hz (handle negative frequencies) */
        float peak_frac = tone_parabolic_peak(tt->magnitudes, peak_bin, TONE_FFT_SIZE);
        float measured_hz;
        if (peak_bin < TONE_FFT_SIZE / 2) {
            measured_hz = peak_frac * TONE_HZ_PER_BIN;
        } else {
            measured_hz = (peak_frac - TONE_FFT_SIZE) * TONE_HZ_PER_BIN;
        }

        /* Estimate noise floor (away from carrier) */
        float noise_floor = tone_estimate_noise_floor(tt->magnitudes, TONE_FFT_SIZE, 0, SEARCH_BINS + 5);
        tt->noise_floor_linear = noise_floor;  /* Store for marker detector baseline */
        tt->snr_db = 20.0f * log10f(peak_mag / (noise_floor + 1e-10f));
        tt->valid = (tt->snr_db >= MIN_SNR_DB);

        if (tt->valid) {
            tt->measured_hz = measured_hz;
            tt->offset_hz = measured_hz;  /* Offset from 0 Hz */
            tt->offset_ppm = (tt->offset_hz / 1.0f) * (CARRIER_NOMINAL_HZ / 1e6f);  /* PPM relative to carrier */
        } else {
            tt->measured_hz = 0.0f;
            tt->offset_hz = 0.0f;
            tt->offset_ppm = 0.0f;
        }
        return;
    }

    /* Normal case for 500/600 Hz tones */

    /* Find expected bin locations */
    int nominal_bin = (int)(tt->nominal_hz / TONE_HZ_PER_BIN + 0.5f);
    int lsb_center = TONE_FFT_SIZE - nominal_bin;

    /* Find USB peak (positive frequency) */
    int usb_peak_bin = tone_find_peak_bin(tt->magnitudes,
                                          nominal_bin - SEARCH_BINS,
                                          nominal_bin + SEARCH_BINS,
                                          TONE_FFT_SIZE);
    float usb_peak_frac = tone_parabolic_peak(tt->magnitudes, usb_peak_bin, TONE_FFT_SIZE);
    float usb_peak_mag = tt->magnitudes[usb_peak_bin];

    /* Find LSB peak (negative frequency) */
    int lsb_peak_bin = tone_find_peak_bin(tt->magnitudes,
                                          lsb_center - SEARCH_BINS,
                                          lsb_center + SEARCH_BINS,
                                          TONE_FFT_SIZE);
    float lsb_peak_frac = tone_parabolic_peak(tt->magnitudes, lsb_peak_bin, TONE_FFT_SIZE);
    float lsb_peak_mag = tt->magnitudes[lsb_peak_bin];

    /* Estimate noise floor */
    float noise_floor = tone_estimate_noise_floor(tt->magnitudes, TONE_FFT_SIZE,
                                                   nominal_bin, SEARCH_BINS + 5);
    tt->noise_floor_linear = noise_floor;  /* Store for marker detector baseline */

    /* Calculate SNR (use stronger sideband) */
    float peak_mag = (usb_peak_mag > lsb_peak_mag) ? usb_peak_mag : lsb_peak_mag;
    tt->snr_db = 20.0f * log10f(peak_mag / (noise_floor + 1e-10f));

    /* Validity check */
    tt->valid = (tt->snr_db >= MIN_SNR_DB);

    if (tt->valid) {
        /* Sideband spacing method for best accuracy */
        float usb_hz = usb_peak_frac * TONE_HZ_PER_BIN;
        float lsb_hz = (TONE_FFT_SIZE - lsb_peak_frac) * TONE_HZ_PER_BIN;

        /* Average both sidebands */
        tt->measured_hz = (usb_hz + lsb_hz) / 2.0f;
        tt->offset_hz = tt->measured_hz - tt->nominal_hz;

        /* Scale to carrier PPM (offset at tone freq → offset at carrier) */
        tt->offset_ppm = (tt->offset_hz / tt->nominal_hz) * (CARRIER_NOMINAL_HZ / 1e6f);
    } else {
        tt->measured_hz = tt->nominal_hz;
        tt->offset_hz = 0.0f;
        tt->offset_ppm = 0.0f;
    }
}

/*============================================================================
 * Logging
 *============================================================================*/

void tone_log_measurement(tone_tracker_t *tt) {
    if (!tt->csv_file) return;

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    float timestamp_ms = tt->frame_count * TONE_FRAME_MS;

    fprintf(tt->csv_file, "%s,%.1f,%.3f,%.3f,%.2f,%.1f,%s\n",
            time_str,
            timestamp_ms,
            tt->measured_hz,
            tt->offset_hz,
            tt->offset_ppm,
            tt->snr_db,
            tt->valid ? "YES" : "NO");
    fflush(tt->csv_file);
}
