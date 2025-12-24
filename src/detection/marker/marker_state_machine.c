/**
 * @file marker_state_machine.c
 * @brief WWV minute marker detection state machine
 *
 * Implements 3-state FSM for marker pulse detection:
 *   - IDLE: Monitoring sliding window accumulator
 *   - IN_MARKER: Measuring pulse duration
 *   - COOLDOWN: Preventing re-triggering after pulse
 *
 * Uses sliding window accumulator over 1-second window to detect
 * 800ms pulses at 1000Hz. Self-tracking baseline proven in v133.
 */

#include "detection/marker_internal.h"
#include "telemetry.h"
#include <stdio.h>
#include <math.h>

/*============================================================================
 * Accumulator Logic
 *============================================================================*/

/**
 * Update sliding window accumulator
 * Maintains sum of energy over last MARKER_WINDOW_FRAMES
 */
static void update_accumulator(marker_detector_t *md, float energy) {
    if (md->history_count >= MARKER_WINDOW_FRAMES) {
        md->accumulated_energy -= md->energy_history[md->history_idx];
    }

    md->energy_history[md->history_idx] = energy;
    md->accumulated_energy += energy;

    md->history_idx = (md->history_idx + 1) % MARKER_WINDOW_FRAMES;
    if (md->history_count < MARKER_WINDOW_FRAMES) {
        md->history_count++;
    }
}

/*============================================================================
 * State Machine
 *============================================================================*/

/**
 * Run detection state machine
 * Called once per FFT frame from marker_detector_process_sample()
 */
void marker_state_machine_run(marker_detector_t *md) {
    float energy = md->current_energy;
    uint64_t frame = md->frame_count;

    update_accumulator(md, energy);

    /* Debug logging - every 20th frame (~100ms) */
    if (md->debug_file && (frame % 20 == 0)) {
        char time_str[16];
        marker_get_wall_time_str(md, frame * FRAME_DURATION_MS, time_str, sizeof(time_str));
        const char *state_names[] = {"IDLE", "IN_MARKER", "COOLDOWN"};
        float ratio = (md->baseline_energy > 0.001f) ? md->accumulated_energy / md->baseline_energy : 0.0f;
        fprintf(md->debug_file, "%s,%.1f,%s,%.1f,%.1f,%.1f,%.4f,%.2f\n",
                time_str, frame * FRAME_DURATION_MS, state_names[md->state],
                md->accumulated_energy, md->baseline_energy, md->threshold,
                energy, ratio);
        fflush(md->debug_file);
    }

    /* Warmup phase - fast adaptation to learn baseline */
    if (!md->warmup_complete) {
        md->baseline_energy += MARKER_WARMUP_ADAPT_RATE * (md->accumulated_energy - md->baseline_energy);
        md->threshold = md->baseline_energy * md->threshold_multiplier;

        if (frame >= md->start_frame + MARKER_WARMUP_FRAMES) {
            md->warmup_complete = true;
            printf("[MARKER] Warmup complete. Baseline=%.1f, Thresh=%.1f, Accum=%.1f\n",
                   md->baseline_energy, md->threshold, md->accumulated_energy);
        }
        return;
    }

    /* No markers in first few seconds - baseline still stabilizing */
    float timestamp_ms = md->frame_count * FRAME_DURATION_MS;
    if (timestamp_ms < MARKER_MIN_STARTUP_MS) {
        md->baseline_energy += md->noise_adapt_rate * (md->accumulated_energy - md->baseline_energy);
        md->threshold = md->baseline_energy * md->threshold_multiplier;
        return;
    }

    /* Self-track baseline during IDLE (proven approach from v133) */
    if (md->state == STATE_IDLE) {
        md->baseline_energy += md->noise_adapt_rate * (md->accumulated_energy - md->baseline_energy);
        if (md->baseline_energy < 0.001f) md->baseline_energy = 0.001f;
        md->threshold = md->baseline_energy * md->threshold_multiplier;
    }

    /* State machine */
    switch (md->state) {
        case STATE_IDLE:
            if (md->accumulated_energy > md->threshold) {
                md->state = STATE_IN_MARKER;
                md->marker_start_frame = frame;
                md->marker_peak_energy = md->accumulated_energy;
                md->marker_duration_frames = 1;
            }
            break;

        case STATE_IN_MARKER:
            md->marker_duration_frames++;
            if (md->accumulated_energy > md->marker_peak_energy) {
                md->marker_peak_energy = md->accumulated_energy;
            }

            /* Check for timeout */
            float duration_ms = md->marker_duration_frames * FRAME_DURATION_MS;
            bool timed_out = (duration_ms > MARKER_MAX_DURATION_MS);

            if (md->accumulated_energy < md->threshold || timed_out) {
                if (duration_ms >= md->min_duration_ms && duration_ms < MARKER_MAX_DURATION_MS) {
                    /* Valid marker! */
                    md->markers_detected++;
                    md->flash_frames_remaining = MARKER_FLASH_FRAMES;

                    float timestamp_ms = frame * FRAME_DURATION_MS;
                    float since_last = (md->last_marker_frame > 0) ?
                        (md->marker_start_frame - md->last_marker_frame) * FRAME_DURATION_MS / 1000.0f : 0.0f;

                    printf("[%7.1fs] *** MINUTE MARKER #%d ***  dur=%.0fms  since=%.1fs  accum=%.2f\n",
                           timestamp_ms / 1000.0f, md->markers_detected,
                           duration_ms, since_last, md->marker_peak_energy);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    marker_get_wall_time_str(md, timestamp_ms, time_str, sizeof(time_str));
                    wwv_time_t wwv = md->wwv_clock ? wwv_clock_now(md->wwv_clock) : (wwv_time_t){0};

                    if (md->csv_file) {
                        fprintf(md->csv_file, "%s,%.1f,M%d,%d,%s,%.6f,%.1f,%.1f,%.6f,%.6f\n",
                                time_str, timestamp_ms, md->markers_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                md->marker_peak_energy, duration_ms, since_last,
                                md->baseline_energy, md->threshold);
                        fflush(md->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_MARKERS, "%s,%.1f,M%d,%d,%s,%.6f,%.1f,%.1f,%.6f,%.6f",
                                time_str, timestamp_ms, md->markers_detected, wwv.second,
                                wwv_event_name(wwv.expected_event),
                                md->marker_peak_energy, duration_ms, since_last,
                                md->baseline_energy, md->threshold);

                    md->last_marker_frame = md->marker_start_frame;

                    /* Callback */
                    if (md->callback) {
                        marker_event_t event = {
                            .marker_number = md->markers_detected,
                            .timestamp_ms = timestamp_ms,
                            .since_last_marker_sec = since_last,
                            .accumulated_energy = md->accumulated_energy,
                            .peak_energy = md->marker_peak_energy,
                            .duration_ms = duration_ms
                        };
                        md->callback(&event, md->callback_user_data);
                    }
                } else if (timed_out) {
                    printf("[%7.1fs] MARKER timed out after %.0fms\n",
                           frame * FRAME_DURATION_MS / 1000.0f, duration_ms);
                }

                md->state = STATE_COOLDOWN;
                md->cooldown_frames = MS_TO_FRAMES(MARKER_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--md->cooldown_frames <= 0) {
                md->state = STATE_IDLE;
            }
            break;
    }
}
