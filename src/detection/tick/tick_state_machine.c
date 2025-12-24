/**
 * @file tick_state_machine.c
 * @brief WWV tick detection state machine
 *
 * Implements 3-state FSM for tick pulse detection:
 *   - IDLE: Monitoring for threshold crossings
 *   - IN_TICK: Measuring pulse duration and peak energy
 *   - COOLDOWN: Preventing re-triggering after pulse
 *
 * Includes adaptive threshold, timing gate, marker classification.
 */

#include "detection/tick_internal.h"
#include "telemetry.h"
#include <stdio.h>
#include <math.h>

/*============================================================================
 * Timing Gate Logic
 *============================================================================*/

/**
 * Check if timing gate is open (tick expected in this window)
 */
bool tick_state_is_gate_open(tick_detector_t *td, float current_ms) {
    if (!td->gate.enabled) {
        return true;  /* Gate disabled - always open */
    }

    if (td->gate.recovery_mode) {
        return true;  /* Recovery mode - bypass gate */
    }

    /* Calculate position within current second */
    float ms_into_second = fmodf(current_ms - td->gate.epoch_ms, 1000.0f);
    if (ms_into_second < 0.0f) ms_into_second += 1000.0f;

    return (ms_into_second >= TICK_GATE_START_MS && ms_into_second <= TICK_GATE_END_MS);
}

/*============================================================================
 * State Machine
 *============================================================================*/

/**
 * Run detection state machine
 * Called once per FFT frame from tick_detector_process_sample()
 */
void tick_state_machine_run(tick_detector_t *td) {
    float energy = td->current_energy;
    uint64_t frame = td->frame_count;

    /* Warmup phase - fast adaptation to establish baseline */
    if (!td->warmup_complete) {
        td->noise_floor += TICK_WARMUP_ADAPT_RATE * (energy - td->noise_floor);
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        td->threshold_high = td->noise_floor * td->threshold_multiplier;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;

        if (frame >= td->start_frame + TICK_WARMUP_FRAMES) {
            td->warmup_complete = true;
            printf("[TICK] Warmup complete. Noise=%.4f, Thresh=%.4f\n",
                   td->noise_floor, td->threshold_high);
        }
        return;
    }

    /* Gate recovery check - if gating enabled but no ticks for too long, enter recovery mode */
    if (td->gate.enabled && !td->gate.recovery_mode && td->state == STATE_IDLE) {
        float since_last_gated_tick_ms = (td->gate.last_tick_frame_gated > 0) ?
            (frame - td->gate.last_tick_frame_gated) * FRAME_DURATION_MS : 0.0f;
        if (td->gate.last_tick_frame_gated > 0 && since_last_gated_tick_ms >= GATE_RECOVERY_MS) {
            td->gate.recovery_mode = true;
            printf("[TICK] Gate recovery mode ENABLED (%.1fs without tick)\n",
                   since_last_gated_tick_ms / 1000.0f);
        }
    }

    /* Adaptive noise floor - asymmetric: fast down, slow up */
    if (td->state == STATE_IDLE && energy < td->threshold_high) {
        if (energy < td->noise_floor) {
            td->noise_floor = td->noise_floor * td->adapt_alpha_down + energy * (1.0f - td->adapt_alpha_down);
        } else {
            td->noise_floor = td->noise_floor * td->adapt_alpha_up + energy * (1.0f - td->adapt_alpha_up);
        }
        if (td->noise_floor < 0.0001f) td->noise_floor = 0.0001f;
        if (td->noise_floor > NOISE_FLOOR_MAX) td->noise_floor = NOISE_FLOOR_MAX;
        td->threshold_high = td->noise_floor * td->threshold_multiplier;
        td->threshold_low = td->threshold_high * TICK_HYSTERESIS_RATIO;
    }

    /* State machine */
    switch (td->state) {
        case STATE_IDLE:
            if (energy > td->threshold_high) {
                /* Check timing gate before transitioning */
                float current_ms = frame * FRAME_DURATION_MS;
                if (!tick_state_is_gate_open(td, current_ms)) {
                    /* Gate closed - ignore this detection (BCD harmonic) */
                    break;
                }

                td->state = STATE_IN_TICK;
                td->tick_start_frame = frame;
                td->tick_peak_energy = energy;
                td->tick_duration_frames = 1;
                td->corr_peak = 0.0f;  /* Reset correlation peak for new detection */
                td->corr_sum = 0.0f;   /* Reset accumulated correlation */
                td->corr_sum_count = 0;
            }
            break;

        case STATE_IN_TICK:
            td->tick_duration_frames++;
            if (energy > td->tick_peak_energy) {
                td->tick_peak_energy = energy;
            }

            if (energy < td->threshold_low) {
                /* Signal dropped - classify based on duration */
                float duration_ms = td->tick_duration_frames * FRAME_DURATION_MS;
                float interval_ms = (td->last_tick_frame > 0) ?
                    (td->tick_start_frame - td->last_tick_frame) * FRAME_DURATION_MS : 0.0f;
                float timestamp_ms = frame * FRAME_DURATION_MS;
                float corr_ratio = (td->corr_noise_floor > 0.001f) ?
                    td->corr_peak / td->corr_noise_floor : 0.0f;

                bool valid_correlation = (td->corr_peak > td->corr_noise_floor * CORR_THRESHOLD_MULT);

                /* Check for minute marker first (600-900ms duration, 55+ seconds since last) */
                bool is_marker_duration = (duration_ms >= MARKER_MIN_DURATION_MS &&
                                           duration_ms <= MARKER_MAX_DURATION_MS_CHECK);

                /* Marker interval check with startup/recovery handling:
                 * - First marker (last_marker_frame == 0): always allow
                 * - Subsequent markers: must be 55+ seconds apart
                 * This handles startup and recovery from fading (missed markers)
                 */
                float since_last_marker_ms = (td->last_marker_frame > 0) ?
                    (td->tick_start_frame - td->last_marker_frame) * FRAME_DURATION_MS : MARKER_MIN_INTERVAL_MS + 1000.0f;
                bool valid_marker_interval = (since_last_marker_ms >= MARKER_MIN_INTERVAL_MS);

                if (is_marker_duration && valid_marker_interval) {
                    /* MINUTE MARKER detected! */
                    td->markers_detected++;
                    td->flash_frames_remaining = TICK_FLASH_FRAMES * 6;  /* Long flash for marker */

                    /* Calculate leading edge (on-time marker).
                     * Leading edge = trailing edge - duration - filter delay.
                     * timestamp_ms is when energy dropped below threshold (trailing edge).
                     * The actual WWV marker START is the on-time reference. */
                    float leading_edge_ms = timestamp_ms - duration_ms - TICK_FILTER_DELAY_MS;

                    printf("[%7.1fs] *** MINUTE MARKER #%-3d ***  dur=%.0fms  corr=%.1f  since=%.1fs  start=%.1fms\n",
                           timestamp_ms / 1000.0f, td->markers_detected,
                           duration_ms, corr_ratio, since_last_marker_ms / 1000.0f, leading_edge_ms);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    tick_get_wall_time_str(td, timestamp_ms, time_str, sizeof(time_str));
                    wwv_time_t wwv = td->wwv_clock ? wwv_clock_now(td->wwv_clock) : (wwv_time_t){0};

                    if (td->csv_file) {
                        fprintf(td->csv_file, "%s,%.1f,M%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f\n",
                                time_str, timestamp_ms, td->markers_detected,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, 0.0f,
                                td->noise_floor, td->corr_peak, corr_ratio);
                        fflush(td->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_TICKS, "%s,%.1f,M%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f",
                                time_str, timestamp_ms, td->markers_detected,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, 0.0f,
                                td->noise_floor, td->corr_peak, corr_ratio);

                    td->last_marker_frame = td->tick_start_frame;
                    /* Don't update last_tick_frame - marker shouldn't affect tick timing */

                    /* Marker callback */
                    if (td->marker_callback) {
                        tick_marker_event_t event = {
                            .marker_number = td->markers_detected,
                            .timestamp_ms = timestamp_ms,
                            .start_timestamp_ms = leading_edge_ms,  /* LEADING EDGE - on-time marker */
                            .duration_ms = duration_ms,
                            .corr_ratio = corr_ratio,
                            .interval_ms = since_last_marker_ms
                        };
                        td->marker_callback(&event, td->marker_callback_user_data);
                    }

                } else if (duration_ms >= td->min_duration_ms &&
                           duration_ms <= TICK_MAX_DURATION_MS &&
                           valid_correlation) {
                    /* Normal tick */
                    td->ticks_detected++;
                    td->flash_frames_remaining = TICK_FLASH_FRAMES;

                    /* Update gated tick tracking for recovery logic */
                    if (td->gate.enabled) {
                        td->gate.last_tick_frame_gated = frame;
                        if (td->gate.recovery_mode) {
                            td->gate.recovery_mode = false;
                            printf("[TICK] Gate recovery mode DISABLED (tick acquired)\n");
                        }
                    }

                    float avg_interval_ms = tick_calculate_avg_interval(td, timestamp_ms);

                    /* Update history */
                    td->tick_timestamps_ms[td->tick_history_idx] = timestamp_ms;
                    td->tick_history_idx = (td->tick_history_idx + 1) % TICK_HISTORY_SIZE;
                    if (td->tick_history_count < TICK_HISTORY_SIZE) {
                        td->tick_history_count++;
                    }

                    /* Console output */
                    char indicator = (interval_ms > 950.0f && interval_ms < 1050.0f) ? ' ' : '!';
                    printf("[%7.1fs] TICK #%-4d  int=%6.0fms  avg=%6.0fms  corr=%.1f %c\n",
                           timestamp_ms / 1000.0f, td->ticks_detected,
                           interval_ms, avg_interval_ms, corr_ratio, indicator);

                    /* CSV logging and telemetry */
                    char time_str[16];
                    tick_get_wall_time_str(td, timestamp_ms, time_str, sizeof(time_str));
                    wwv_time_t wwv = td->wwv_clock ? wwv_clock_now(td->wwv_clock) : (wwv_time_t){0};

                    if (td->csv_file) {
                        fprintf(td->csv_file, "%s,%.1f,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f\n",
                                time_str, timestamp_ms, td->ticks_detected,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, avg_interval_ms,
                                td->noise_floor, td->corr_peak, corr_ratio);
                        fflush(td->csv_file);
                    }

                    /* UDP telemetry */
                    telem_sendf(TELEM_TICKS, "%s,%.1f,%d,%s,%.6f,%.1f,%.0f,%.0f,%.6f,%.2f,%.1f",
                                time_str, timestamp_ms, td->ticks_detected,
                                wwv_event_name(wwv.expected_event),
                                td->tick_peak_energy, duration_ms, interval_ms, avg_interval_ms,
                                td->noise_floor, td->corr_peak, corr_ratio);

                    td->last_tick_frame = td->tick_start_frame;

                    /* Callback */
                    if (td->callback) {
                        tick_event_t event = {
                            .tick_number = td->ticks_detected,
                            .timestamp_ms = timestamp_ms,
                            .interval_ms = interval_ms,
                            .duration_ms = duration_ms,
                            .peak_energy = td->tick_peak_energy,
                            .avg_interval_ms = avg_interval_ms,
                            .noise_floor = td->noise_floor,
                            .corr_peak = td->corr_peak,
                            .corr_ratio = corr_ratio
                        };
                        td->callback(&event, td->callback_user_data);
                    }
                } else {
                    /* Rejected - duration in the gap zone (50-600ms) or failed other checks */
                    td->ticks_rejected++;
                    if (duration_ms > TICK_MAX_DURATION_MS && duration_ms < MARKER_MIN_DURATION_MS) {
                        printf("[%7.1fs] REJECTED: dur=%.0fms (gap zone 50-600ms)\n",
                               timestamp_ms / 1000.0f, duration_ms);
                    } else if (is_marker_duration && !valid_marker_interval) {
                        printf("[%7.1fs] REJECTED: dur=%.0fms (marker-like but only %.1fs since last marker)\n",
                               timestamp_ms / 1000.0f, duration_ms, since_last_marker_ms / 1000.0f);
                    }
                }

                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);

            } else if (td->tick_duration_frames * FRAME_DURATION_MS > MARKER_MAX_DURATION_MS) {
                /* Pulse WAY too long (>1s) - something is wrong, bail out */
                td->ticks_rejected++;
                printf("[%7.1fs] REJECTED: pulse >1s, bailing out\n",
                       frame * FRAME_DURATION_MS / 1000.0f);
                td->state = STATE_COOLDOWN;
                td->cooldown_frames = MS_TO_FRAMES(TICK_COOLDOWN_MS);
            }
            break;

        case STATE_COOLDOWN:
            if (--td->cooldown_frames <= 0) {
                td->state = STATE_IDLE;
            }
            break;
    }
}
