/**
 * @file bcd_freq_detector.c
 * @brief WWV BCD frequency-domain detector implementation
 *
 * Self-contained module with:
 *   - Own 2048-point FFT (40.96ms frames for precise frequency isolation)
 *   - Sliding window accumulator
 *   - Self-tracking baseline
 *   - CSV logging
 *
 * Pattern: Follows marker_detector.c structure
 *
 * This detector provides confident 100Hz identification.
 * Works in parallel with bcd_time_detector which provides precise edge timing.
 * The bcd_correlator combines both for reliable symbol output.
 */

#include "bcd_freq_detector.h"
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

bcd_freq_detector_t *bcd_freq_detector_create(const char *csv_path) {
    bcd_freq_detector_t *fd = (bcd_freq_detector_t *)calloc(1, sizeof(bcd_freq_detector_t));
    if (!fd) return NULL;

    fd->fft = fft_processor_create(BCD_FREQ_FFT_SIZE, BCD_FREQ_SAMPLE_RATE);
    if (!fd->fft) {
        free(fd);
        return NULL;
    }

    float frame_duration_ms = bcd_freq_detector_get_frame_duration_ms();
    int window_frames = (int)(BCD_FREQ_WINDOW_MS / frame_duration_ms);

    fd->i_buffer = (float *)malloc(BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->q_buffer = (float *)malloc(BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->energy_history = (float *)malloc(window_frames * sizeof(float));

    if (!fd->i_buffer || !fd->q_buffer || !fd->energy_history) {
        bcd_freq_detector_destroy(fd);
        return NULL;
    }

    memset(fd->i_buffer, 0, BCD_FREQ_FFT_SIZE * sizeof(float));
    memset(fd->q_buffer, 0, BCD_FREQ_FFT_SIZE * sizeof(float));
    fd->buffer_idx = 0;

    memset(fd->energy_history, 0, window_frames * sizeof(float));
    fd->history_idx = 0;
    fd->history_count = 0;
    fd->accumulated_energy = 0.0f;
    fd->baseline_energy = 0.0001f;

    fd->state = STATE_IDLE;
    fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
    fd->detection_enabled = true;
    fd->warmup_complete = false;
    fd->start_time = time(NULL);

    if (csv_path) {
        fd->csv_file = fopen(csv_path, "w");
        if (fd->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(fd->csv_file, "# Phoenix SDR BCD Freq Detector Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(fd->csv_file, "# Started: %s\n", time_str);
            fprintf(fd->csv_file, "# FFT: %d (%.2fms), Window: %d frames (%.0fms)\n",
                    BCD_FREQ_FFT_SIZE, frame_duration_ms, window_frames, BCD_FREQ_WINDOW_MS);
            fprintf(fd->csv_file, "# Target: %dHz ±%dHz\n",
                    BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);
            fprintf(fd->csv_file, "time,timestamp_ms,pulse_num,accum_energy,duration_ms,baseline,snr_db\n");
            fflush(fd->csv_file);
        }
    }

    printf("[BCD_FREQ] Detector created: FFT=%d (%.2fms), window=%d frames (%.0fms)\n",
           BCD_FREQ_FFT_SIZE, frame_duration_ms, window_frames, BCD_FREQ_WINDOW_MS);
    printf("[BCD_FREQ] Target: %dHz ±%dHz, self-tracking baseline\n",
           BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);

    return fd;
}

void bcd_freq_detector_destroy(bcd_freq_detector_t *fd) {
    if (!fd) return;

    if (fd->csv_file) fclose(fd->csv_file);
    if (fd->fft) fft_processor_destroy(fd->fft);
    free(fd->i_buffer);
    free(fd->q_buffer);
    free(fd->energy_history);
    free(fd);
}

void bcd_freq_detector_set_callback(bcd_freq_detector_t *fd,
                                    bcd_freq_callback_fn callback,
                                    void *user_data) {
    if (!fd) return;
    fd->callback = callback;
    fd->callback_user_data = user_data;
}

bool bcd_freq_detector_process_sample(bcd_freq_detector_t *fd,
                                      float i_sample,
                                      float q_sample) {
    if (!fd || !fd->detection_enabled) return false;

    /* Buffer sample for FFT */
    fd->i_buffer[fd->buffer_idx] = i_sample;
    fd->q_buffer[fd->buffer_idx] = q_sample;
    fd->buffer_idx++;

    /* Not enough samples yet */
    if (fd->buffer_idx < BCD_FREQ_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    fd->buffer_idx = 0;

    /* Run FFT */
    fft_processor_process(fd->fft, fd->i_buffer, fd->q_buffer);

    /* Extract bucket energy */
    fd->current_energy = bcd_freq_calculate_bucket_energy(fd);

    /* Run detection state machine */
    bcd_freq_run_state_machine(fd);

    fd->frame_count++;

    return (fd->state == STATE_IN_PULSE && fd->pulse_duration_frames == 1);
}

void bcd_freq_detector_set_enabled(bcd_freq_detector_t *fd, bool enabled) {
    if (fd) fd->detection_enabled = enabled;
}

bool bcd_freq_detector_get_enabled(bcd_freq_detector_t *fd) {
    return fd ? fd->detection_enabled : false;
}

float bcd_freq_detector_get_accumulated_energy(bcd_freq_detector_t *fd) {
    return fd ? fd->accumulated_energy : 0.0f;
}

float bcd_freq_detector_get_baseline(bcd_freq_detector_t *fd) {
    return fd ? fd->baseline_energy : 0.0f;
}

float bcd_freq_detector_get_threshold(bcd_freq_detector_t *fd) {
    return fd ? fd->threshold : 0.0f;
}

float bcd_freq_detector_get_current_energy(bcd_freq_detector_t *fd) {
    return fd ? fd->current_energy : 0.0f;
}

int bcd_freq_detector_get_pulse_count(bcd_freq_detector_t *fd) {
    return fd ? fd->pulses_detected : 0;
}

void bcd_freq_detector_print_stats(bcd_freq_detector_t *fd) {
    if (!fd) return;

    float frame_duration_ms = bcd_freq_detector_get_frame_duration_ms();
    int window_frames = (int)(BCD_FREQ_WINDOW_MS / frame_duration_ms);
    float elapsed = fd->frame_count * frame_duration_ms / 1000.0f;

    printf("\n=== BCD FREQ DETECTOR STATS ===\n");
    printf("FFT: %d (%.2fms), Window: %d frames (%.0fms)\n",
           BCD_FREQ_FFT_SIZE, frame_duration_ms, window_frames, BCD_FREQ_WINDOW_MS);
    printf("Target: %d Hz ±%d Hz\n", BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Rejected: %d\n",
           elapsed, fd->pulses_detected, fd->pulses_rejected);
    printf("Baseline: %.6f  Threshold: %.6f  Accumulated: %.6f\n",
           fd->baseline_energy, fd->threshold, fd->accumulated_energy);
    printf("===============================\n");
}

float bcd_freq_detector_get_frame_duration_ms(void) {
    return (float)BCD_FREQ_FFT_SIZE * 1000.0f / BCD_FREQ_SAMPLE_RATE;
}
