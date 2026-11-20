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
 * Pulse estimate
 *============================================================================*/

/**
 * Recover the pulse from the window-sum excursion.
 *
 * The detector thresholds a W-frame sliding sum, so it reports the run of that
 * sum above threshold, not the pulse: a rectangular pulse of length L gives a
 * run of D = W + L - 2*f*min(L, W), where f = (T - b)/(peak - b) is the
 * threshold's fraction of the plateau above baseline. That is the 800 ms marker
 * read as 1.6-1.8 s.
 *
 * The run's midpoint is start + (W + L)/2 whatever f is, and a WWV minute marker
 * is 800 ms, so the leading edge is midpoint - (W + 800)/2. The length estimate
 * solves D for L; it is ill-conditioned as f nears 0.5 and is for reporting only.
 */
static void marker_estimate_pulse(const marker_detector_t *md, uint64_t end_frame,
                                  float *start_ms, float *length_ms) {
    const float W = MARKER_WINDOW_FRAMES * FRAME_DURATION_MS;
    float up = md->marker_start_frame * FRAME_DURATION_MS;
    float down = end_frame * FRAME_DURATION_MS;
    float D = down - up;

    *start_ms = 0.5f * (up + down) - 0.5f * (W + MARKER_PULSE_MS);

    float plateau = md->marker_peak_energy - md->baseline_energy;
    float f = (plateau > 0.0f) ? (md->threshold - md->baseline_energy) / plateau : 0.5f;
    float g = 1.0f - 2.0f * f;
    float L = (fabsf(g) > 1e-3f) ? (D - W) / g : -1.0f;   /* case L <= W */
    if (L < 0.0f || L > W) L = D - W * g;                 /* case L > W */
    if (L < 0.0f) L = 0.0f;
    if (L > D) L = D;
    *length_ms = L;
}

/*============================================================================
 * Tone purity
 *============================================================================*/

/* Track the centre-bin and flank-bin backgrounds at the given rate. Called
 * wherever baseline_energy adapts, so both freeze while a run is measured. */
static void track_purity_background(marker_detector_t *md, float rate) {
    int slot = (int)(md->frame_count % MARKER_PURITY_RING);
    md->bg_centre += rate * (md->centre_ring[slot] - md->bg_centre);
    md->bg_flank += rate * (md->flank_ring[slot] - md->bg_flank);
}

/**
 * Flank-to-centre ratio of the run's excess over background, measured over the
 * estimated 800 ms pulse. About 0.04 for a pure 1000 Hz tone at any SNR (Hann
 * leakage two bins out), about 1.0 for broadband audio such as voice.
 */
static float marker_flank_ratio(const marker_detector_t *md, uint64_t end_frame, float pulse_start_ms) {
    long n = (long)(MARKER_PULSE_MS / FRAME_DURATION_MS + 0.5f);
    long first = (long)(pulse_start_ms / FRAME_DURATION_MS + 0.5f);
    long oldest = (long)end_frame - MARKER_PURITY_RING + 1;
    if (first < oldest) first = oldest;
    if (first < 0) first = 0;
    if (first + n > (long)end_frame + 1) n = (long)end_frame + 1 - first;
    if (n <= 0) return INFINITY;

    double centre = 0.0, flank = 0.0;
    for (long f = first; f < first + n; f++) {
        int slot = (int)(f % MARKER_PURITY_RING);
        centre += md->centre_ring[slot];
        flank += md->flank_ring[slot];
    }
    float centre_excess = (float)(centre / n) - md->bg_centre;
    float flank_excess = (float)(flank / n) - md->bg_flank;
    if (centre_excess <= 0.0f) return INFINITY;
    return flank_excess / centre_excess;
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
        track_purity_background(md, MARKER_WARMUP_ADAPT_RATE);

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
        track_purity_background(md, md->noise_adapt_rate);
        return;
    }

    /* Logging: the block's largest window sum over the baseline it was judged
     * against, in every state (the threshold is threshold_multiplier times it) */
    if (md->baseline_energy > 0.0f) {
        float r = md->accumulated_energy / md->baseline_energy;
        if (r > md->block_peak_ratio) {
            md->block_peak_ratio = r;
            md->block_peak_frame = frame;
        }
    }
    if ((frame - md->block_start_frame) * FRAME_DURATION_MS >= MARKER_PEAK_LOG_MS) {
        printf("[%7.1fs] MARKER block peak: sum/baseline %.2f at %.1fms (threshold %.2f) state=%d\n",
               frame * FRAME_DURATION_MS / 1000.0f, md->block_peak_ratio,
               md->block_peak_frame * FRAME_DURATION_MS, md->threshold_multiplier, (int)md->state);
        md->block_peak_ratio = 0.0f;
        md->block_start_frame = frame;
    }

    /* Self-track baseline during IDLE (proven approach from v133) */
    if (md->state == STATE_IDLE) {
        md->baseline_energy += md->noise_adapt_rate * (md->accumulated_energy - md->baseline_energy);
        if (md->baseline_energy < 0.001f) md->baseline_energy = 0.001f;
        md->threshold = md->baseline_energy * md->threshold_multiplier;
        track_purity_background(md, md->noise_adapt_rate);
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
                float pulse_start_ms, pulse_ms;
                marker_estimate_pulse(md, frame, &pulse_start_ms, &pulse_ms);
                bool long_enough = (duration_ms >= md->min_duration_ms && duration_ms < MARKER_MAX_DURATION_MS);
                md->last_flank_ratio = long_enough ?
                    marker_flank_ratio(md, frame, pulse_start_ms) : 0.0f;

                if (long_enough && !(md->last_flank_ratio <= MARKER_MAX_FLANK_RATIO)) {
                    /* Not a tone: broadband audio (the :52.5 voice announcement)
                     * in the 1000 Hz bucket. No cooldown, so the real marker that
                     * follows within seconds is still seen. */
                    printf("[%7.1fs] MARKER rejected: not a tone, flank/centre excess %.2f > %.2f "
                           "(window run %.0fms, start=%.1fms)\n",
                           frame * FRAME_DURATION_MS / 1000.0f, md->last_flank_ratio,
                           MARKER_MAX_FLANK_RATIO, duration_ms, pulse_start_ms);
                    md->state = STATE_IDLE;
                    break;
                }

                if (long_enough) {
                    /* Valid marker! */
                    md->markers_detected++;
                    md->flash_frames_remaining = MARKER_FLASH_FRAMES;

                    float timestamp_ms = frame * FRAME_DURATION_MS;
                    float since_last = (md->last_marker_frame > 0) ?
                        (md->marker_start_frame - md->last_marker_frame) * FRAME_DURATION_MS / 1000.0f : 0.0f;

                    printf("[%7.1fs] *** MINUTE MARKER #%d ***  dur=%.0fms (window run %.0fms)  start=%.1fms  since=%.1fs  accum=%.2f  flank=%.2f  peak/baseline %.2f\n",
                           timestamp_ms / 1000.0f, md->markers_detected,
                           pulse_ms, duration_ms, pulse_start_ms, since_last, md->marker_peak_energy,
                           md->last_flank_ratio, md->marker_peak_energy / md->baseline_energy);

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
                            .duration_ms = pulse_ms,
                            .start_timestamp_ms = pulse_start_ms
                        };
                        md->callback(&event, md->callback_user_data);
                    }
                } else if (!timed_out) {
                    /* Too short to be a marker, so no cooldown: like a purity
                     * rejection, it must not hide the marker that follows. A
                     * blip 13 s before :00 took the 30 s cooldown and hid the
                     * kiwi agc 11:43 marker. */
                    printf("[%7.1fs] MARKER short run: window run %.0fms < %.0fms, start=%.1fms "
                           "peak/baseline %.2f\n",
                           frame * FRAME_DURATION_MS / 1000.0f, duration_ms, md->min_duration_ms,
                           pulse_start_ms, md->marker_peak_energy / md->baseline_energy);
                    md->state = STATE_IDLE;
                    break;
                } else {
                    printf("[%7.1fs] MARKER timed out after %.0fms - resetting baseline\n",
                           frame * FRAME_DURATION_MS / 1000.0f, duration_ms);
                    /* Restored from before 7e7805e: without it, sustained in-band
                     * energy keeps the detector cycling IN_MARKER -> timeout. */
                    md->baseline_energy = md->accumulated_energy;
                    md->threshold = md->baseline_energy * md->threshold_multiplier;
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
