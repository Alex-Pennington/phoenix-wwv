/**
 * @file tone_tracker.c
 * @brief WWV tone frequency tracker implementation
 *
 * Measures exact frequency of 500/600 Hz reference tones using:
 * - Both sidebands (USB + LSB) for accuracy
 * - Parabolic interpolation for sub-bin resolution
 * - SNR gating for validity
 */

#include "tone_tracker.h"
#include "detection/tone/tone_tracker_internal.h"
#include "fft_processor.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/*============================================================================
 * Public API
 *============================================================================*/

tone_tracker_t *tone_tracker_create(float nominal_hz, const char *csv_path) {
    tone_tracker_t *tt = (tone_tracker_t *)calloc(1, sizeof(tone_tracker_t));
    if (!tt) return NULL;

    tt->nominal_hz = nominal_hz;
    tt->start_time = time(NULL);

    /* Allocate buffers */
    tt->buffer_i = (float *)calloc(TONE_FFT_SIZE, sizeof(float));
    tt->buffer_q = (float *)calloc(TONE_FFT_SIZE, sizeof(float));
    tt->magnitudes = (float *)malloc(TONE_FFT_SIZE * sizeof(float));

    if (!tt->buffer_i || !tt->buffer_q || !tt->magnitudes) {
        tone_tracker_destroy(tt);
        return NULL;
    }

    /* Initialize FFT */
    tt->fft = fft_processor_create(TONE_FFT_SIZE, TONE_SAMPLE_RATE);
    if (!tt->fft) {
        tone_tracker_destroy(tt);
        return NULL;
    }

    /* Open CSV file */
    if (csv_path) {
        tt->csv_file = fopen(csv_path, "w");
        if (tt->csv_file) {
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S",
                     localtime(&tt->start_time));

            fprintf(tt->csv_file, "# Phoenix SDR WWV Tone Tracker (%.0f Hz) v%s\n",
                    nominal_hz, PHOENIX_VERSION_FULL);
            fprintf(tt->csv_file, "# Started: %s\n", time_str);
            fprintf(tt->csv_file, "# FFT: %d-pt, %.2f Hz/bin, %.1f ms frame\n",
                    TONE_FFT_SIZE, TONE_HZ_PER_BIN, TONE_FRAME_MS);
            fprintf(tt->csv_file, "time,timestamp_ms,measured_hz,offset_hz,offset_ppm,snr_db,valid\n");
            fflush(tt->csv_file);
        }
    }

    printf("[TONE] Tracker created for %.0f Hz (%.2f Hz/bin, %.1f ms frame)\n",
           nominal_hz, TONE_HZ_PER_BIN, TONE_FRAME_MS);

    return tt;
}

void tone_tracker_destroy(tone_tracker_t *tt) {
    if (!tt) return;

    if (tt->csv_file) fclose(tt->csv_file);
    if (tt->fft) fft_processor_destroy(tt->fft);
    free(tt->buffer_i);
    free(tt->buffer_q);
    free(tt->magnitudes);
    free(tt);
}

void tone_tracker_process_sample(tone_tracker_t *tt, float i, float q) {
    if (!tt) return;

    /* Store sample in circular buffer */
    tt->buffer_i[tt->buffer_idx] = i;
    tt->buffer_q[tt->buffer_idx] = q;
    tt->buffer_idx = (tt->buffer_idx + 1) % TONE_FFT_SIZE;
    tt->samples_collected++;

    /* Process when buffer is full */
    if (tt->samples_collected >= TONE_FFT_SIZE) {
        tt->samples_collected = 0;

        tone_measure_frequency(tt);
        tone_log_measurement(tt);

        tt->frame_count++;
    }
}

float tone_tracker_get_measured_hz(tone_tracker_t *tt) {
    return tt ? tt->measured_hz : 0.0f;
}

float tone_tracker_get_offset_hz(tone_tracker_t *tt) {
    return tt ? tt->offset_hz : 0.0f;
}

float tone_tracker_get_offset_ppm(tone_tracker_t *tt) {
    return tt ? tt->offset_ppm : 0.0f;
}

float tone_tracker_get_snr_db(tone_tracker_t *tt) {
    return tt ? tt->snr_db : 0.0f;
}

bool tone_tracker_is_valid(tone_tracker_t *tt) {
    return tt ? tt->valid : false;
}

uint64_t tone_tracker_get_frame_count(tone_tracker_t *tt) {
    return tt ? tt->frame_count : 0;
}

float tone_tracker_get_noise_floor(tone_tracker_t *tt) {
    return tt ? tt->noise_floor_linear : 0.0f;
}

void tone_tracker_update_global_noise_floor(tone_tracker_t *tt) {
    if (!tt || !tt->valid) return;

    /* Only update if this tracker has a valid measurement */
    if (tt->noise_floor_linear > 0.0001f) {
        /* Slow adaptation to prevent jumps */
        g_subcarrier_noise_floor += 0.1f * (tt->noise_floor_linear - g_subcarrier_noise_floor);
        if (g_subcarrier_noise_floor < 0.0001f) g_subcarrier_noise_floor = 0.0001f;
    }
}

/*============================================================================
 * Global Variables
 *============================================================================*/

/* Global subcarrier noise floor for marker detector */
float g_subcarrier_noise_floor = 0.01f;
