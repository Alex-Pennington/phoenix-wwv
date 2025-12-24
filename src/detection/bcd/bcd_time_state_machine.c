/**
 * @file bcd_time_state_machine.c
 * @brief BCD time detector state machine implementation
 *
 * Extracted from bcd_time_detector.c to separate detection logic
 * from API management.
 *
 * Contains:
 *   - 3-state FSM (IDLE → IN_PULSE → COOLDOWN)
 *   - Adaptive noise floor tracking
 *   - Pulse duration measurement
 *   - CSV logging and telemetry
 *   - Event callbacks
 *
 * Hot path: bcd_time_run_state_machine() called once per FFT frame
 */

#include "bcd_internal.h"
#include "telemetry.h"
#include "version.h"
#include <math.h>
#include <string.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)BCD_TIME_FFT_SIZE * 1000.0f / BCD_TIME_SAMPLE_RATE)

/* Detection timing */
#define BCD_TIME_COOLDOWN_MS        200.0f

/* Threshold adaptation */
#define BCD_TIME_NOISE_ADAPT_DOWN   0.002f
#define BCD_TIME_NOISE_ADAPT_UP     0.0002f
#define BCD_TIME_WARMUP_ADAPT_RATE  0.05f
#define BCD_TIME_WARMUP_FRAMES      50

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

/*============================================================================
 * Internal Functions
 *============================================================================*/

float bcd_time_calculate_bucket_energy(bcd_time_detector_t *td) {
    return fft_processor_get_bucket_energy(td->fft, BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
}

void bcd_time_run_state_machine(bcd_time_detector_t *td) {
    float energy = td->current_energy;
    uint64_t frame = td->frame_count;

    /* Warmup phase - fast adaptation to establish baseline */
    if (!td->warmup_complete) {
        td->noise_floor += BCD_TIME_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < NOISE_FLOOR_MIN) td->noise_floor = NOISE_FLOOR_MIN;
        td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;

        if (frame >= td->start_frame + BCD_TIME_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[BCD_TIME] Warmup complete. Noise=%.6f, Thresh=%.6f\n",
                   td->noise_floor, td->threshold_high);
        }
        return;
    }

    /* Adaptive noise floor - asymmetric: fast down, slow up */
    if (td->state == STATE_IDLE && energy < td->threshold_high) {
        if (energy < td->noise_floor) {
            td->noise_floor += BCD_TIME_NOISE_ADAPT_DOWN * (energy - td->noise_floor);
        } else {
            td->noise_floor += BCD_TIME_NOISE_ADAPT_UP * (energy - td->noise_floor);
        }
        if (td->noise_floor < NOISE_FLOOR_MIN) td->noise_floor = NOISE_FLOOR_MIN;
        if (td->noise_floor > NOISE_FLOOR_MAX) td->noise_floor = NOISE_FLOOR_MAX;
        td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
    }

    /* State machine */
    switch (td->state) {
        case STATE_IDLE:
            if (energy > td->threshold_high) {
                td->state = STATE_IN_PULSE;
                td->pulse_start_frame = frame;
                td->pulse_peak_energy = energy;
                td->pulse_duration_frames = 1;
                td->consecutive_low_frames = 0;
            }
            break;

        case STATE_IN_PULSE:
            td->pulse_duration_frames++;
            if (energy > td->pulse_peak_energy) {
                td->pulse_peak_energy = energy;
            }

            /* Phase 9: Require consecutive low frames before ending pulse */
            if (energy < td->threshold_low) {
                td->consecutive_low_frames++;
            } else {
                td->consecutive_low_frames = 0;  /* Reset if signal returns */
            }

            if (td->consecutive_low_frames >= MIN_LOW_FRAMES) {
                /* Pulse ended - check validity */
                float duration_ms = td->pulse_duration_frames * FRAME_DURATION_MS;
                float timestamp_ms = td->pulse_start_frame * FRAME_DURATION_MS;
                float snr_db = 10.0f * log10f(td->pulse_peak_energy / td->noise_floor);

                if (duration_ms >= BCD_TIME_PULSE_MIN_MS &&
                    duration_ms <= BCD_TIME_PULSE_MAX_MS) {
                    /* Valid pulse! */
                    td->pulses_detected++;

                    printf("[BCD_TIME] Pulse #%d at %.1fms  dur=%.0fms  SNR=%.1fdB\n",
                           td->pulses_detected, timestamp_ms, duration_ms, snr_db);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    bcd_get_wall_time_str(td->start_time, timestamp_ms, time_str, sizeof(time_str));

                    if (td->csv_file) {
                        fprintf(td->csv_file, "%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f\n",
                                time_str, timestamp_ms, td->pulses_detected,
                                td->pulse_peak_energy, duration_ms,
                                td->noise_floor, snr_db);
                        fflush(td->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_BCDS, "TIME,%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f",
                                time_str, timestamp_ms, td->pulses_detected,
                                td->pulse_peak_energy, duration_ms,
                                td->noise_floor, snr_db);

                    td->last_pulse_frame = td->pulse_start_frame;

                    /* Callback */
                    if (td->callback) {
                        bcd_time_event_t event = {
                            .timestamp_ms = timestamp_ms,
                            .duration_ms = duration_ms,
                            .peak_energy = td->pulse_peak_energy,
                            .noise_floor = td->noise_floor,
                            .snr_db = snr_db
                        };
                        td->callback(&event, td->callback_user_data);
                    }
                } else {
                    /* Rejected pulse */
                    td->pulses_rejected++;
                    if (duration_ms < BCD_TIME_PULSE_MIN_MS) {
                        /* Too short - likely noise, don't log */
                    } else {
                        printf("[BCD_TIME] Rejected: dur=%.0fms (>%.0fms max)\n",
                               duration_ms, BCD_TIME_PULSE_MAX_MS);
                    }
                }

                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(BCD_TIME_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--td->cooldown_frames <= 0) {
                td->state = STATE_IDLE;
            }
            break;
    }
}
