/**
 * @file bcd_time_detector.c
 * @brief WWV BCD time-domain detector implementation
 *
 * Self-contained module with:
 *   - Own 256-point FFT (5.12ms frames for precise edge detection)
 *   - Own sample buffer
 *   - Adaptive threshold state machine
 *   - CSV logging
 *
 * Pattern: Follows tick_detector.c structure
 *
 * This detector provides precise pulse edge timestamps for 100Hz BCD pulses.
 * Works in parallel with bcd_freq_detector which provides confident 100Hz
 * identification. The bcd_correlator combines both for reliable symbol output.
 */

#include "bcd_time_detector.h"
#include "bcd_internal.h"
#include "telemetry.h"
#include "fft_processor.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Public API Implementation
 *============================================================================*/

bcd_time_detector_t *bcd_time_detector_create(const char *csv_path) {
    bcd_time_detector_t *td = (bcd_time_detector_t *)calloc(1, sizeof(bcd_time_detector_t));
    if (!td) return NULL;

    /* Allocate FFT */
    td->fft = fft_processor_create(BCD_TIME_FFT_SIZE, BCD_TIME_SAMPLE_RATE);
    if (!td->fft) {
        free(td);
        return NULL;
    }

    td->i_buffer = (float *)malloc(BCD_TIME_FFT_SIZE * sizeof(float));
    td->q_buffer = (float *)malloc(BCD_TIME_FFT_SIZE * sizeof(float));

    if (!td->i_buffer || !td->q_buffer) {
        bcd_time_detector_destroy(td);
        return NULL;
    }

    /* Initialize buffers */
    memset(td->i_buffer, 0, BCD_TIME_FFT_SIZE * sizeof(float));
    memset(td->q_buffer, 0, BCD_TIME_FFT_SIZE * sizeof(float));
    td->buffer_idx = 0;

    /* Initialize state */
    td->state = STATE_IDLE;
    td->noise_floor = 0.0001f;
    td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
    td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->warmup_complete = false;
    td->start_time = time(NULL);

    /* Open CSV file */
    if (csv_path) {
        td->csv_file = fopen(csv_path, "w");
        if (td->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            float frame_duration = bcd_time_detector_get_frame_duration_ms();
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(td->csv_file, "# Phoenix SDR BCD Time Detector Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(td->csv_file, "# Started: %s\n", time_str);
            fprintf(td->csv_file, "# FFT: %d (%.2fms), Target: %dHz ±%dHz\n",
                    BCD_TIME_FFT_SIZE, frame_duration,
                    BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
            fprintf(td->csv_file, "time,timestamp_ms,pulse_num,peak_energy,duration_ms,noise_floor,snr_db\n");
            fflush(td->csv_file);
        }
    }

    float frame_duration = bcd_time_detector_get_frame_duration_ms();
    printf("[BCD_TIME] Detector created: FFT=%d (%.2fms), Target=%dHz ±%dHz\n",
           BCD_TIME_FFT_SIZE, frame_duration,
           BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);

    return td;
}

void bcd_time_detector_destroy(bcd_time_detector_t *td) {
    if (!td) return;

    if (td->csv_file) fclose(td->csv_file);
    if (td->fft) fft_processor_destroy(td->fft);
    free(td->i_buffer);
    free(td->q_buffer);
    free(td);
}

void bcd_time_detector_set_callback(bcd_time_detector_t *td,
                                    bcd_time_callback_fn callback,
                                    void *user_data) {
    if (!td) return;
    td->callback = callback;
    td->callback_user_data = user_data;
}

bool bcd_time_detector_process_sample(bcd_time_detector_t *td,
                                      float i_sample,
                                      float q_sample) {
    if (!td || !td->detection_enabled) return false;

    /* Buffer sample for FFT */
    td->i_buffer[td->buffer_idx] = i_sample;
    td->q_buffer[td->buffer_idx] = q_sample;
    td->buffer_idx++;

    /* Not enough samples yet */
    if (td->buffer_idx < BCD_TIME_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    td->buffer_idx = 0;

    /* Run FFT */
    fft_processor_process(td->fft, td->i_buffer, td->q_buffer);

    /* Extract bucket energy */
    td->current_energy = bcd_time_calculate_bucket_energy(td);

    /* Run detection state machine */
    bcd_time_run_state_machine(td);

    td->frame_count++;

    return (td->state == STATE_IN_PULSE && td->pulse_duration_frames == 1);
}

void bcd_time_detector_set_enabled(bcd_time_detector_t *td, bool enabled) {
    if (td) td->detection_enabled = enabled;
}

bool bcd_time_detector_get_enabled(bcd_time_detector_t *td) {
    return td ? td->detection_enabled : false;
}

float bcd_time_detector_get_noise_floor(bcd_time_detector_t *td) {
    return td ? td->noise_floor : 0.0f;
}

float bcd_time_detector_get_threshold(bcd_time_detector_t *td) {
    return td ? td->threshold_high : 0.0f;
}

float bcd_time_detector_get_current_energy(bcd_time_detector_t *td) {
    return td ? td->current_energy : 0.0f;
}

int bcd_time_detector_get_pulse_count(bcd_time_detector_t *td) {
    return td ? td->pulses_detected : 0;
}

void bcd_time_detector_print_stats(bcd_time_detector_t *td) {
    if (!td) return;

    float frame_duration = bcd_time_detector_get_frame_duration_ms();
    float elapsed = td->frame_count * frame_duration / 1000.0f;

    printf("\n=== BCD TIME DETECTOR STATS ===\n");
    printf("FFT: %d (%.2fms), Target: %d Hz ±%d Hz\n",
           BCD_TIME_FFT_SIZE, frame_duration,
           BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Rejected: %d\n",
           elapsed, td->pulses_detected, td->pulses_rejected);
    printf("Noise floor: %.6f  Threshold: %.6f\n",
           td->noise_floor, td->threshold_high);
    printf("===============================\n");
}

float bcd_time_detector_get_frame_duration_ms(void) {
    return (float)BCD_TIME_FFT_SIZE * 1000.0f / BCD_TIME_SAMPLE_RATE;
}
