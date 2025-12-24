/**
 * @file tick_detector.c
 * @brief WWV tick pulse detector implementation
 *
 * Self-contained module with:
 *   - Own 256-point FFT (5.3ms frames for 5ms pulse detection)
 *   - Own sample buffer
 *   - Adaptive threshold state machine
 *   - CSV logging
 *
 * This is the pattern for all detection channels:
 *   1. Size FFT for target signal timing
 *   2. Buffer samples until FFT ready
 *   3. Extract energy in target frequency bucket
 *   4. Run detection state machine
 *   5. Report events via callback
 */

#include "tick_detector.h"
#include "detection/tick_internal.h"
#include "telemetry.h"
#include "version.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Helper Functions
 *============================================================================*/

static float calculate_bucket_energy(tick_detector_t *td) {
    return fft_processor_get_bucket_energy(td->fft, TICK_TARGET_FREQ_HZ, TICK_BANDWIDTH_HZ);
}

float tick_calculate_avg_interval(tick_detector_t *td, float current_time_ms) {
    if (td->tick_history_count < 2) return 0.0f;

    float cutoff = current_time_ms - TICK_AVG_WINDOW_MS;
    float sum = 0.0f;
    int count = 0;
    float prev_time = -1.0f;

    for (int i = 0; i < td->tick_history_count; i++) {
        int idx = (td->tick_history_idx - td->tick_history_count + i + TICK_HISTORY_SIZE) % TICK_HISTORY_SIZE;
        float t = td->tick_timestamps_ms[idx];
        if (t >= cutoff) {
            if (prev_time >= 0.0f) {
                sum += (t - prev_time);
                count++;
            }
            prev_time = t;
        }
    }

    return (count > 0) ? (sum / count) : 0.0f;
}

/**
 * Get wall clock time string for CSV output
 * Format: HH:MM:SS
 */
void tick_get_wall_time_str(tick_detector_t *td, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = td->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

tick_detector_t *tick_detector_create(const char *csv_path) {
    tick_detector_t *td = (tick_detector_t *)calloc(1, sizeof(tick_detector_t));
    if (!td) return NULL;

    /* Allocate FFT */
    td->fft = fft_processor_create(TICK_FFT_SIZE, TICK_SAMPLE_RATE);
    if (!td->fft) {
        free(td);
        return NULL;
    }

    td->i_buffer = (float *)malloc(TICK_FFT_SIZE * sizeof(float));
    td->q_buffer = (float *)malloc(TICK_FFT_SIZE * sizeof(float));

    /* Allocate matched filter resources */
    td->template_i = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->template_q = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->corr_buf_i = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));
    td->corr_buf_q = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));

    if (!td->i_buffer || !td->q_buffer ||
        !td->template_i || !td->template_q || !td->corr_buf_i || !td->corr_buf_q) {
        tick_detector_destroy(td);
        return NULL;
    }

    /* Initialize buffers */
    memset(td->i_buffer, 0, TICK_FFT_SIZE * sizeof(float));
    memset(td->q_buffer, 0, TICK_FFT_SIZE * sizeof(float));
    td->buffer_idx = 0;

    /* Initialize matched filter */
    tick_correlation_init(td);

    /* Initialize tunable parameters to defaults */
    td->threshold_multiplier = TICK_THRESHOLD_MULT;      /* 2.0 */
    td->adapt_alpha_down = 1.0f - TICK_NOISE_ADAPT_DOWN; /* 0.998 */
    td->adapt_alpha_up = 1.0f - TICK_NOISE_ADAPT_UP;     /* 0.9998 */
    td->min_duration_ms = TICK_MIN_DURATION_MS;          /* 2.0 */

    /* Initialize state */
    td->state = STATE_IDLE;
    td->noise_floor = 0.01f;
    td->threshold_high = td->noise_floor * td->threshold_multiplier;
    td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    td->detection_enabled = true;
    td->warmup_complete = false;
    td->start_time = time(NULL);  /* Record wall clock start time */

    /* Initialize timing gate (disabled until marker sets epoch) */
    td->gate.epoch_ms = 0.0f;
    td->gate.enabled = false;
    td->epoch_source = EPOCH_SOURCE_NONE;
    td->epoch_confidence = 0.0f;

    /* Create WWV clock tracker */
    td->wwv_clock = wwv_clock_create(WWV_STATION_WWV);

    /* Create comb filter */
    td->comb_filter = comb_create();
    if (!td->comb_filter) {
        tick_detector_destroy(td);
        return NULL;
    }

    /* Open CSV file */
    if (csv_path) {
        td->csv_file = fopen(csv_path, "w");
        if (td->csv_file) {
            /* Version and timestamp header */
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(td->csv_file, "# Phoenix SDR WWV Tick Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(td->csv_file, "# Started: %s\n", time_str);
            fprintf(td->csv_file, "time,timestamp_ms,tick_num,expected,energy_peak,duration_ms,interval_ms,avg_interval_ms,noise_floor,corr_peak,corr_ratio\n");
            fflush(td->csv_file);
        }
    }

    printf("[TICK] Detector created: FFT=%d (%.1fms), matched filter=%d samples (%.1fms)\n",
           TICK_FFT_SIZE, FRAME_DURATION_MS, TICK_TEMPLATE_SAMPLES, TICK_PULSE_MS);
    printf("[TICK] Target: %dHz ±%dHz, logging to %s\n",
           TICK_TARGET_FREQ_HZ, TICK_BANDWIDTH_HZ, csv_path ? csv_path : "(disabled)");

    return td;
}

void tick_detector_destroy(tick_detector_t *td) {
    if (!td) return;

    if (td->wwv_clock) wwv_clock_destroy(td->wwv_clock);
    if (td->comb_filter) comb_destroy(td->comb_filter);
    if (td->csv_file) fclose(td->csv_file);
    fft_processor_destroy(td->fft);
    free(td->i_buffer);
    free(td->q_buffer);
    free(td->template_i);
    free(td->template_q);
    free(td->corr_buf_i);
    free(td->corr_buf_q);
    free(td);
}

void tick_detector_set_callback(tick_detector_t *td, tick_callback_fn callback, void *user_data) {
    if (!td) return;
    td->callback = callback;
    td->callback_user_data = user_data;
}

void tick_detector_set_marker_callback(tick_detector_t *td, tick_marker_callback_fn callback, void *user_data) {
    if (!td) return;
    td->marker_callback = callback;
    td->marker_callback_user_data = user_data;
}

bool tick_detector_process_sample(tick_detector_t *td, float i_sample, float q_sample) {
    if (!td || !td->detection_enabled) return false;

    /* Always feed correlation buffer (sample-by-sample) */
    td->corr_buf_i[td->corr_buf_idx] = i_sample;
    td->corr_buf_q[td->corr_buf_idx] = q_sample;
    td->corr_buf_idx = (td->corr_buf_idx + 1) % TICK_CORR_BUFFER_SIZE;
    td->corr_sample_count++;

    /* Compute correlation every N samples (for efficiency) */
    if (td->corr_sample_count >= TICK_TEMPLATE_SAMPLES &&
        (td->corr_sample_count % CORR_DECIMATION) == 0) {
        float corr = tick_correlation_compute(td);

        /* Update correlation noise floor (slow adaptation) */
        if (corr < td->corr_noise_floor || td->corr_noise_floor < 0.001f) {
            td->corr_noise_floor += CORR_NOISE_ADAPT * (corr - td->corr_noise_floor);
        } else if (td->state == STATE_IDLE) {
            td->corr_noise_floor += (CORR_NOISE_ADAPT * 0.1f) * (corr - td->corr_noise_floor);
        }

        /* Track peak during detection */
        if (td->state == STATE_IN_TICK && corr > td->corr_peak) {
            td->corr_peak = corr;
            td->corr_peak_offset = td->corr_sample_count;
        }

        /* Accumulate correlation during detection */
        if (td->state == STATE_IN_TICK) {
            td->corr_sum += corr;
            td->corr_sum_count++;
        }
    }

    /* Buffer sample for FFT */
    td->i_buffer[td->buffer_idx] = i_sample;
    td->q_buffer[td->buffer_idx] = q_sample;
    td->buffer_idx++;

    /* Not enough samples yet */
    if (td->buffer_idx < TICK_FFT_SIZE) {
        return false;
    }

    /* Buffer full - run FFT */
    td->buffer_idx = 0;

    /* Run FFT */
    fft_processor_process(td->fft, td->i_buffer, td->q_buffer);

    /* Extract bucket energy */
    td->current_energy = calculate_bucket_energy(td);

    /* Run detection state machine */
    tick_state_machine_run(td);

    td->frame_count++;

    return (td->flash_frames_remaining == TICK_FLASH_FRAMES);
}

int tick_detector_get_flash_frames(tick_detector_t *td) {
    return td ? td->flash_frames_remaining : 0;
}

void tick_detector_decrement_flash(tick_detector_t *td) {
    if (td && td->flash_frames_remaining > 0) {
        td->flash_frames_remaining--;
    }
}

void tick_detector_set_enabled(tick_detector_t *td, bool enabled) {
    if (td) td->detection_enabled = enabled;
}

bool tick_detector_get_enabled(tick_detector_t *td) {
    return td ? td->detection_enabled : false;
}

float tick_detector_get_noise_floor(tick_detector_t *td) {
    return td ? td->noise_floor : 0.0f;
}

float tick_detector_get_threshold(tick_detector_t *td) {
    return td ? td->threshold_high : 0.0f;
}

float tick_detector_get_current_energy(tick_detector_t *td) {
    return td ? td->current_energy : 0.0f;
}

float tick_detector_get_threshold_mult(tick_detector_t *td) {
    return td ? td->threshold_multiplier : TICK_THRESHOLD_MULT;
}

float tick_detector_get_adapt_alpha_down(tick_detector_t *td) {
    return td ? td->adapt_alpha_down : (1.0f - TICK_NOISE_ADAPT_DOWN);
}

float tick_detector_get_adapt_alpha_up(tick_detector_t *td) {
    return td ? td->adapt_alpha_up : (1.0f - TICK_NOISE_ADAPT_UP);
}

float tick_detector_get_min_duration_ms(tick_detector_t *td) {
    return td ? td->min_duration_ms : TICK_MIN_DURATION_MS;
}

int tick_detector_get_tick_count(tick_detector_t *td) {
    return td ? td->ticks_detected : 0;
}

void tick_detector_print_stats(tick_detector_t *td) {
    if (!td) return;

    float elapsed = td->frame_count * FRAME_DURATION_MS / 1000.0f;
    float current_time_ms = td->frame_count * FRAME_DURATION_MS;
    float detecting = td->warmup_complete ?
        (elapsed - TICK_WARMUP_FRAMES * FRAME_DURATION_MS / 1000.0f) : 0.0f;
    int expected = (int)detecting;
    float rate = (expected > 0) ? (100.0f * td->ticks_detected / expected) : 0.0f;
    float avg_interval = tick_calculate_avg_interval(td, current_time_ms);

    printf("\n=== TICK DETECTOR STATS ===\n");
    printf("FFT: %d (%.1fms), Matched filter: %d samples\n", TICK_FFT_SIZE, FRAME_DURATION_MS, TICK_TEMPLATE_SAMPLES);
    printf("Target: %d Hz +/-%d Hz\n", TICK_TARGET_FREQ_HZ, TICK_BANDWIDTH_HZ);
    printf("Elapsed: %.1fs  Detected: %d  Expected: %d  Rate: %.1f%%\n",
           elapsed, td->ticks_detected, expected, rate);
    printf("Markers: %d  Rejected: %d  Avg interval: %.0fms\n",
           td->markers_detected, td->ticks_rejected, avg_interval);
    printf("Energy noise: %.4f  Corr noise: %.2f\n", td->noise_floor, td->corr_noise_floor);
    printf("===========================\n");
}

void tick_detector_log_metadata(tick_detector_t *td, uint64_t center_freq,
                                uint32_t sample_rate, uint32_t gain_reduction,
                                uint32_t lna_state) {
    if (!td || !td->csv_file) return;

    /* Get current wall clock time */
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    /* Get timestamp in ms since detector start */
    float timestamp_ms = td->frame_count * FRAME_DURATION_MS;

    /* Log as special META row */
    fprintf(td->csv_file, "%s,%.1f,META,0,freq=%llu rate=%u GR=%u LNA=%u,0,0,0,0,0,0\n",
            time_str, timestamp_ms,
            (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
    fflush(td->csv_file);

    printf("[TICK] Logged metadata: freq=%llu, rate=%u, GR=%u, LNA=%u\n",
           (unsigned long long)center_freq, sample_rate, gain_reduction, lna_state);
}

void tick_detector_log_display_gain(tick_detector_t *td, float display_gain_db) {
    if (!td || !td->csv_file) return;

    /* Get current wall clock time */
    char time_str[64];
    time_t now = time(NULL);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));

    /* Get timestamp in ms since detector start */
    float timestamp_ms = td->frame_count * FRAME_DURATION_MS;

    /* Log as special GAIN row */
    fprintf(td->csv_file, "%s,%.1f,GAIN,0,display_gain=%.1f,0,0,0,0,0,0,0\n",
            time_str, timestamp_ms, display_gain_db);
    fflush(td->csv_file);
}

float tick_detector_get_frame_duration_ms(void) {
    return FRAME_DURATION_MS;
}

/*============================================================================
 * Timing Gate API (Step 2: WWV Tick/BCD Separation)
 *============================================================================*/

void tick_detector_set_epoch_with_source(tick_detector_t *td, float epoch_ms,
                                          epoch_source_t source, float confidence) {
    if (!td) return;

    /* Normalize to millisecond within second (0-999) */
    float normalized_epoch = fmodf(epoch_ms, 1000.0f);
    if (normalized_epoch < 0) {
        normalized_epoch += 1000.0f;
    }

    td->gate.epoch_ms = normalized_epoch;
    td->epoch_source = source;
    td->epoch_confidence = confidence;

    /* Log epoch updates to console telemetry */
    const char *source_str = (source == EPOCH_SOURCE_TICK_CHAIN) ? "CHAIN" :
                             (source == EPOCH_SOURCE_MARKER) ? "MARKER" : "UNKNOWN";
    telem_console("[EPOCH] Set from %s: offset=%.1fms confidence=%.3f\n",
                  source_str, normalized_epoch, confidence);
}

void tick_detector_set_epoch(tick_detector_t *td, float epoch_ms) {
    /* Legacy function - assume marker source with medium confidence */
    tick_detector_set_epoch_with_source(td, epoch_ms, EPOCH_SOURCE_MARKER, 0.7f);
}

void tick_detector_set_gating_enabled(tick_detector_t *td, bool enabled) {
    if (!td) return;
    td->gate.enabled = enabled;
    if (enabled) {
        /* Initialize recovery tracking when gate is enabled */
        td->gate.last_tick_frame_gated = td->frame_count;  /* Start counting from now */
        td->gate.recovery_mode = false;
        printf("[TICK] Timing gate ENABLED (window: %.0f-%.0fms into second)\n",
               TICK_GATE_START_MS, TICK_GATE_END_MS);
    } else {
        td->gate.recovery_mode = false;
        printf("[TICK] Timing gate DISABLED\n");
    }
}

float tick_detector_get_epoch(tick_detector_t *td) {
    return td ? td->gate.epoch_ms : 0.0f;
}

bool tick_detector_is_gating_enabled(tick_detector_t *td) {
    return td ? td->gate.enabled : false;
}

epoch_source_t tick_detector_get_epoch_source(tick_detector_t *td) {
    return td ? td->epoch_source : EPOCH_SOURCE_NONE;
}

float tick_detector_get_epoch_confidence(tick_detector_t *td) {
    return td ? td->epoch_confidence : 0.0f;
}

/*============================================================================
 * Runtime Tunable Parameters
 *============================================================================*/

bool tick_detector_set_threshold_mult(tick_detector_t *td, float value) {
    if (!td || value < 1.0f || value > 5.0f) return false;
    td->threshold_multiplier = value;
    td->threshold_high = td->noise_floor * td->threshold_multiplier;
    td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    return true;
}

bool tick_detector_set_adapt_alpha_down(tick_detector_t *td, float value) {
    if (!td || value < 0.9f || value > 0.999f) return false;
    td->adapt_alpha_down = value;
    return true;
}

bool tick_detector_set_adapt_alpha_up(tick_detector_t *td, float value) {
    if (!td || value < 0.001f || value > 0.1f) return false;
    td->adapt_alpha_up = value;
    return true;
}

bool tick_detector_set_min_duration_ms(tick_detector_t *td, float value) {
    if (!td || value < 1.0f || value > 10.0f) return false;
    td->min_duration_ms = value;
    return true;
}
