/**
 * @file marker_detector.c
 * @brief WWV minute marker detector - public API and coordination
 *
 * This module provides the public API for marker detection and coordinates
 * the detection pipeline. The core detection logic has been extracted to:
 *   - marker_internal.h: Shared state and configuration
 *   - marker_state_machine.c: Detection state machine
 *
 * Responsibilities:
 *   - FFT processing and energy extraction
 *   - Public API functions
 *   - Resource management
 */

#include "marker_detector.h"
#include "detection/marker_internal.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*============================================================================
 * Helper Functions
 *============================================================================*/

static float calculate_bucket_energy(marker_detector_t *md) {
    return fft_processor_get_bucket_energy(md->fft, MARKER_TARGET_FREQ_HZ, MARKER_BANDWIDTH_HZ);
}

void marker_get_wall_time_str(marker_detector_t *md, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = md->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

marker_detector_t *marker_detector_create(const char *csv_path) {
    marker_detector_t *md = (marker_detector_t *)calloc(1, sizeof(marker_detector_t));
    if (!md) return NULL;

    md->fft = fft_processor_create(MARKER_FFT_SIZE, MARKER_SAMPLE_RATE);
    if (!md->fft) {
        free(md);
        return NULL;
    }

    md->i_buffer = (float *)malloc(MARKER_FFT_SIZE * sizeof(float));
    md->q_buffer = (float *)malloc(MARKER_FFT_SIZE * sizeof(float));
    md->energy_history = (float *)malloc(MARKER_WINDOW_FRAMES * sizeof(float));

    if (!md->i_buffer || !md->q_buffer || !md->energy_history) {
        marker_detector_destroy(md);
        return NULL;
    }

    memset(md->i_buffer, 0, MARKER_FFT_SIZE * sizeof(float));
    memset(md->q_buffer, 0, MARKER_FFT_SIZE * sizeof(float));
    md->buffer_idx = 0;

    memset(md->energy_history, 0, MARKER_WINDOW_FRAMES * sizeof(float));
    md->history_idx = 0;
    md->history_count = 0;
    md->accumulated_energy = 0.0f;
    md->baseline_energy = 0.01f;

    md->state = STATE_IDLE;
    md->threshold = md->baseline_energy * MARKER_THRESHOLD_MULT;
    md->detection_enabled = true;
    md->warmup_complete = false;
    md->start_time = time(NULL);

    /* Initialize tunable parameters to defaults */
    md->threshold_multiplier = MARKER_THRESHOLD_MULT;      /* 3.0 */
    md->noise_adapt_rate = MARKER_NOISE_ADAPT_RATE;        /* 0.001 */
    md->min_duration_ms = MARKER_MIN_DURATION_MS;          /* 500.0 */

    md->wwv_clock = wwv_clock_create(WWV_STATION_WWV);

    if (csv_path) {
        md->csv_file = fopen(csv_path, "w");
        if (md->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(md->csv_file, "# Phoenix SDR WWV Marker Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(md->csv_file, "# Started: %s\n", time_str);
            fprintf(md->csv_file, "# Sliding window: %d frames (%.0f ms)\n",
                    MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
            fprintf(md->csv_file, "time,timestamp_ms,marker_num,wwv_sec,expected,accum_energy,duration_ms,since_last_sec,baseline,threshold\n");
            fflush(md->csv_file);
        }

        /* Debug log */
        char debug_path[512];
        strncpy(debug_path, csv_path, sizeof(debug_path) - 1);
        debug_path[sizeof(debug_path) - 1] = '\0';
        char *markers_pos = strstr(debug_path, "markers.csv");
        if (markers_pos) {
            strcpy(markers_pos, "debug_marker.csv");
        } else {
            strcat(debug_path, "_debug.csv");
        }
        md->debug_file = fopen(debug_path, "w");
        if (md->debug_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(md->debug_file, "# Phoenix SDR Marker Debug Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(md->debug_file, "# Started: %s\n", time_str);
            fprintf(md->debug_file, "time,timestamp_ms,state,accum,baseline,threshold,energy,ratio\n");
            fflush(md->debug_file);
            printf("[MARKER] Debug log: %s\n", debug_path);
        }
    }

    printf("[MARKER] Detector created: FFT=%d (%.1fms), window=%d frames (%.0fms)\n",
           MARKER_FFT_SIZE, FRAME_DURATION_MS, MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
    printf("[MARKER] Target: %dHz ±%dHz, self-tracking baseline\n",
           MARKER_TARGET_FREQ_HZ, MARKER_BANDWIDTH_HZ);

    return md;
}

void marker_detector_destroy(marker_detector_t *md) {
    if (!md) return;

    if (md->wwv_clock) wwv_clock_destroy(md->wwv_clock);
    if (md->csv_file) fclose(md->csv_file);
    if (md->debug_file) fclose(md->debug_file);
    if (md->fft) fft_processor_destroy(md->fft);
    free(md->i_buffer);
    free(md->q_buffer);
    free(md->energy_history);
    free(md);
}

void marker_detector_set_callback(marker_detector_t *md, marker_callback_fn callback, void *user_data) {
    if (!md) return;
    md->callback = callback;
    md->callback_user_data = user_data;
}

bool marker_detector_process_sample(marker_detector_t *md, float i_sample, float q_sample) {
    if (!md || !md->detection_enabled) return false;

    md->i_buffer[md->buffer_idx] = i_sample;
    md->q_buffer[md->buffer_idx] = q_sample;
    md->buffer_idx++;

    if (md->buffer_idx < MARKER_FFT_SIZE) {
        return false;
    }

    md->buffer_idx = 0;

    fft_processor_process(md->fft, md->i_buffer, md->q_buffer);
    md->current_energy = calculate_bucket_energy(md);
    marker_state_machine_run(md);
    md->frame_count++;

    return (md->flash_frames_remaining == MARKER_FLASH_FRAMES);
}

int marker_detector_get_flash_frames(marker_detector_t *md) {
    return md ? md->flash_frames_remaining : 0;
}

void marker_detector_decrement_flash(marker_detector_t *md) {
    if (md && md->flash_frames_remaining > 0) {
        md->flash_frames_remaining--;
    }
}

void marker_detector_set_enabled(marker_detector_t *md, bool enabled) {
    if (md) md->detection_enabled = enabled;
}

bool marker_detector_get_enabled(marker_detector_t *md) {
    return md ? md->detection_enabled : false;
}

float marker_detector_get_accumulated_energy(marker_detector_t *md) {
    return md ? md->accumulated_energy : 0.0f;
}

float marker_detector_get_threshold(marker_detector_t *md) {
    return md ? md->threshold : 0.0f;
}

float marker_detector_get_current_energy(marker_detector_t *md) {
    return md ? md->current_energy : 0.0f;
}

int marker_detector_get_marker_count(marker_detector_t *md) {
    return md ? md->markers_detected : 0;
}

void marker_detector_print_stats(marker_detector_t *md) {
    if (!md) return;

    float elapsed = md->frame_count * FRAME_DURATION_MS / 1000.0f;
    int expected_markers = (int)(elapsed / 60.0f);

    printf("\n=== MARKER DETECTOR STATS ===\n");
    printf("FFT: %d (%.1fms), Window: %d frames (%.0fms)\n",
           MARKER_FFT_SIZE, FRAME_DURATION_MS, MARKER_WINDOW_FRAMES, MARKER_WINDOW_MS);
    printf("Target: %d Hz +/-%d Hz\n", MARKER_TARGET_FREQ_HZ, MARKER_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Expected: ~%d\n",
           elapsed, md->markers_detected, expected_markers);
    printf("Baseline: %.4f  Threshold: %.4f\n", md->baseline_energy, md->threshold);
    printf("=============================\n");
}

void marker_detector_log_metadata(marker_detector_t *md, uint64_t center_freq,
                                  uint32_t sample_rate, uint32_t gain_reduction,
                                  uint32_t lna_state) {
    if (!md || !md->csv_file) return;

    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;

    fprintf(md->csv_file, "%s,%.1f,META,0,freq=%llu rate=%u GR=%u LNA=%u,0,0,0,0,0\n",
            time_str, timestamp_ms,
            (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
    fflush(md->csv_file);
}

void marker_detector_log_display_gain(marker_detector_t *md, float display_gain) {
    if (!md || !md->csv_file) return;

    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;

    fprintf(md->csv_file, "%s,%.1f,GAIN,0,display_gain=%+.0fdB,0,0,0,0,0\n",
            time_str, timestamp_ms, display_gain);
    fflush(md->csv_file);
}

float marker_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

void marker_detector_set_threshold_mult(marker_detector_t *md, float mult) {
    if (!md || mult < 2.0f || mult > 5.0f) return;
    md->threshold_multiplier = mult;
    md->threshold = md->baseline_energy * md->threshold_multiplier;
}

float marker_detector_get_threshold_mult(marker_detector_t *md) {
    return md ? md->threshold_multiplier : 3.0f;
}

void marker_detector_set_noise_adapt_rate(marker_detector_t *md, float rate) {
    if (!md || rate < 0.0001f || rate > 0.01f) return;
    md->noise_adapt_rate = rate;
}

float marker_detector_get_noise_adapt_rate(marker_detector_t *md) {
    return md ? md->noise_adapt_rate : 0.001f;
}

void marker_detector_set_min_duration_ms(marker_detector_t *md, float ms) {
    if (!md || ms < 300.0f || ms > 700.0f) return;
    md->min_duration_ms = ms;
}

float marker_detector_get_min_duration_ms(marker_detector_t *md) {
    return md ? md->min_duration_ms : 500.0f;
}
