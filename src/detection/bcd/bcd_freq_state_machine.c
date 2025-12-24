/**
 * @file bcd_freq_state_machine.c
 * @brief BCD frequency detector state machine implementation
 *
 * Extracted from bcd_freq_detector.c to separate detection logic
 * from API management.
 *
 * Contains:
 *   - 3-state FSM (IDLE → IN_PULSE → COOLDOWN)
 *   - Sliding window accumulator
 *   - Self-tracking baseline
 *   - CSV logging and telemetry
 *   - Event callbacks
 *
 * Hot path: bcd_freq_run_state_machine() called once per FFT frame
 */

#include "bcd_internal.h"
#include "telemetry.h"
#include "version.h"
#include <math.h>
#include <string.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)BCD_FREQ_FFT_SIZE * 1000.0f / BCD_FREQ_SAMPLE_RATE)
#define WINDOW_FRAMES       ((int)(BCD_FREQ_WINDOW_MS / FRAME_DURATION_MS))

/* Detection timing */
#define BCD_FREQ_COOLDOWN_MS        500.0f
#define BCD_FREQ_MAX_DURATION_MS    2000.0f

/* Warmup */
#define BCD_FREQ_WARMUP_FRAMES      50
#define BCD_FREQ_WARMUP_ADAPT_RATE  0.02f
#define BCD_FREQ_MIN_STARTUP_MS     5000.0f

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

/*============================================================================
 * Internal Functions
 *============================================================================*/

float bcd_freq_calculate_bucket_energy(bcd_freq_detector_t *fd) {
    return fft_processor_get_bucket_energy(fd->fft, BCD_FREQ_TARGET_FREQ_HZ, BCD_FREQ_BANDWIDTH_HZ);
}

void bcd_freq_update_accumulator(bcd_freq_detector_t *fd, float energy) {
    if (fd->history_count >= WINDOW_FRAMES) {
        fd->accumulated_energy -= fd->energy_history[fd->history_idx];
    }

    fd->energy_history[fd->history_idx] = energy;
    fd->accumulated_energy += energy;

    fd->history_idx = (fd->history_idx + 1) % WINDOW_FRAMES;
    if (fd->history_count < WINDOW_FRAMES) {
        fd->history_count++;
    }
}

void bcd_freq_run_state_machine(bcd_freq_detector_t *fd) {
    float energy = fd->current_energy;
    uint64_t frame = fd->frame_count;

    bcd_freq_update_accumulator(fd, energy);

    /* Warmup phase - fast adaptation to learn baseline */
    if (!fd->warmup_complete) {
        fd->baseline_energy += BCD_FREQ_WARMUP_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;

        if (frame >= fd->start_frame + BCD_FREQ_WARMUP_FRAMES) {
            fd->warmup_complete = true;
            printf("[BCD_FREQ] Warmup complete. Baseline=%.4f, Thresh=%.4f, Accum=%.4f\n",
                   fd->baseline_energy, fd->threshold, fd->accumulated_energy);
        }
        return;
    }

    /* No pulses in first few seconds - baseline still stabilizing */
    float timestamp_ms = fd->frame_count * FRAME_DURATION_MS;
    if (timestamp_ms < BCD_FREQ_MIN_STARTUP_MS) {
        fd->baseline_energy += BCD_FREQ_NOISE_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
        return;
    }

    /* Self-track baseline during IDLE */
    if (fd->state == STATE_IDLE) {
        fd->baseline_energy += BCD_FREQ_NOISE_ADAPT_RATE * (fd->accumulated_energy - fd->baseline_energy);
        if (fd->baseline_energy < 0.0001f) fd->baseline_energy = 0.0001f;
        fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
    }

    /* State machine */
    switch (fd->state) {
        case STATE_IDLE:
            if (fd->accumulated_energy > fd->threshold) {
                fd->state = STATE_IN_PULSE;
                fd->pulse_start_frame = frame;
                fd->pulse_peak_energy = fd->accumulated_energy;
                fd->pulse_duration_frames = 1;
                fd->consecutive_low_frames = 0;
            }
            break;

        case STATE_IN_PULSE:
            fd->pulse_duration_frames++;
            if (fd->accumulated_energy > fd->pulse_peak_energy) {
                fd->pulse_peak_energy = fd->accumulated_energy;
            }

            /* Check for timeout or signal drop */
            float duration_ms = fd->pulse_duration_frames * FRAME_DURATION_MS;
            bool timed_out = (duration_ms > BCD_FREQ_MAX_DURATION_MS);

            /* Phase 9: Require consecutive low frames before ending pulse */
            if (fd->accumulated_energy < fd->threshold) {
                fd->consecutive_low_frames++;
            } else {
                fd->consecutive_low_frames = 0;  /* Reset if signal returns */
            }

            if ((fd->consecutive_low_frames >= MIN_LOW_FRAMES) || timed_out) {
                float start_timestamp_ms = fd->pulse_start_frame * FRAME_DURATION_MS;

                if (duration_ms >= BCD_FREQ_PULSE_MIN_MS &&
                    duration_ms <= BCD_FREQ_PULSE_MAX_MS) {
                    /* Valid pulse! */
                    fd->pulses_detected++;

                    float snr_db = 10.0f * log10f(fd->pulse_peak_energy / fd->baseline_energy);

                    printf("[BCD_FREQ] Pulse #%d at %.1fms  dur=%.0fms  accum=%.4f  SNR=%.1fdB\n",
                           fd->pulses_detected, start_timestamp_ms, duration_ms,
                           fd->pulse_peak_energy, snr_db);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    bcd_get_wall_time_str(fd->start_time, start_timestamp_ms, time_str, sizeof(time_str));

                    if (fd->csv_file) {
                        fprintf(fd->csv_file, "%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f\n",
                                time_str, start_timestamp_ms, fd->pulses_detected,
                                fd->pulse_peak_energy, duration_ms,
                                fd->baseline_energy, snr_db);
                        fflush(fd->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_BCDS, "FREQ,%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f",
                                time_str, start_timestamp_ms, fd->pulses_detected,
                                fd->pulse_peak_energy, duration_ms,
                                fd->baseline_energy, snr_db);

                    fd->last_pulse_frame = fd->pulse_start_frame;

                    /* Callback */
                    if (fd->callback) {
                        bcd_freq_event_t event = {
                            .timestamp_ms = start_timestamp_ms,
                            .duration_ms = duration_ms,
                            .accumulated_energy = fd->pulse_peak_energy,
                            .baseline_energy = fd->baseline_energy,
                            .snr_db = snr_db
                        };
                        fd->callback(&event, fd->callback_user_data);
                    }
                } else if (timed_out) {
                    printf("[BCD_FREQ] Timeout after %.0fms - resetting baseline\n", duration_ms);
                    fd->baseline_energy = fd->accumulated_energy;
                    fd->threshold = fd->baseline_energy * BCD_FREQ_THRESHOLD_MULT;
                    fd->pulses_rejected++;
                } else {
                    fd->pulses_rejected++;
                }

                fd->state = STATE_COOLDOWN;
                fd->cooldown_frames = MS_TO_FRAMES(BCD_FREQ_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--fd->cooldown_frames <= 0) {
                fd->state = STATE_IDLE;
            }
            break;
    }
}
