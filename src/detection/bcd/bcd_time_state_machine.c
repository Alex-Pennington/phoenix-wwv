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
#include <stdlib.h>

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)BCD_TIME_FFT_SIZE * 1000.0f / BCD_TIME_SAMPLE_RATE)
#define TIME_FRAMES_TO_MS(f) wwv_frames_to_ms((f), BCD_TIME_FFT_SIZE, BCD_TIME_SAMPLE_RATE)  /* stream time */

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

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

/* Median of n values (sorts them in place); 0 for none */
static float median_of(float *v, int n) {
    if (n <= 0) return 0.0f;
    qsort(v, (size_t)n, sizeof *v, cmp_float);
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* The step fit (BCD_TIME_FIT_* in bcd_time_detector.h) over the log energy of
 * frames [sf - pre, sf + span). Returns the box's width in ms, or -1 when the
 * window is not held or no box rises above the outside level; *contrast gets
 * exp(inside - outside mean log energy). */
static float fit_width(const bcd_time_detector_t *td, uint64_t sf, float *contrast) {
    const int pre = MS_TO_FRAMES(BCD_TIME_FIT_PRE_MS);
    const int span = MS_TO_FRAMES(BCD_TIME_FIT_SPAN_MS);
    const int rise_max = MS_TO_FRAMES(BCD_TIME_FIT_RISE_MAX_MS);
    const int min_width = (int)(BCD_TIME_FIT_MIN_WIDTH_MS / FRAME_DURATION_MS);
    const int n = pre + span;
    double ps[BCD_TIME_HIST_FRAMES + 1];

    if (sf < (uint64_t)pre + 1 || n > BCD_TIME_HIST_FRAMES) return -1.0f;
    uint64_t b0 = sf - pre;
    ps[0] = 0.0;
    for (int i = 0; i < n; i++) {
        ps[i + 1] = ps[i] + td->log_energy[(b0 + i) % BCD_TIME_HIST_FRAMES];
    }
    double tot = ps[n], best = -1.0, bh = 0.0, bl = 0.0;
    int br = -1, be = -1;
    for (int r = 0; r <= pre + rise_max; r++) {
        for (int e = r + min_width; e <= n - BCD_TIME_FIT_TAIL_FRAMES; e++) {
            double si = ps[e] - ps[r], so = tot - si;
            int ni = e - r, no = n - ni;
            double h = si / ni, l = so / no;
            if (h <= l) continue;
            double f = si * si / ni + so * so / no;   /* least squares: max of this */
            if (best < 0.0 || f > best) { best = f; br = r; be = e; bh = h; bl = l; }
        }
    }
    if (br < 0) return -1.0f;
    *contrast = (float)exp(bh - bl);
    return (be - br) * FRAME_DURATION_MS;
}

/* Print, log and call back one valid event */
static void report_event(bcd_time_detector_t *td, const bcd_time_pending_t *p) {
    wwv_time_ms_t timestamp_ms = TIME_FRAMES_TO_MS(p->start_frame);
    float duration_ms = p->duration_ms;

    if (p->fit) {
        float contrast = 0.0f;
        float w = fit_width(td, p->start_frame, &contrast);
        if (w > 0.0f && contrast >= BCD_TIME_FIT_CONTRAST_MIN &&
            contrast < BCD_TIME_FIT_CONTRAST_MAX) {
            duration_ms = w + BCD_TIME_FIT_OFFSET_MS;
        }
    }

    printf("[BCD_TIME] Pulse #%d at %.1fms  dur=%.0fms  SNR=%.1fdB\n",
           p->pulse_num, timestamp_ms, duration_ms, p->snr_db);

    /* CSV logging and telemetry */
    char time_str[16];
    bcd_get_wall_time_str(td->start_time, timestamp_ms, time_str, sizeof(time_str));

    if (td->csv_file) {
        fprintf(td->csv_file, "%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f\n",
                time_str, timestamp_ms, p->pulse_num,
                p->peak_energy, duration_ms,
                p->noise_floor, p->snr_db);
        fflush(td->csv_file);
    }

    /* UDP telemetry */
    telem_sendf(TELEM_BCDS, "TIME,%s,%.1f,%d,%.6f,%.0f,%.6f,%.1f",
                time_str, timestamp_ms, p->pulse_num,
                p->peak_energy, duration_ms,
                p->noise_floor, p->snr_db);

    if (td->callback) {
        bcd_time_event_t event = {
            .timestamp_ms = timestamp_ms,
            .duration_ms = duration_ms,
            .peak_energy = p->peak_energy,
            .noise_floor = p->noise_floor,
            .snr_db = p->snr_db
        };
        td->callback(&event, td->callback_user_data);
    }
}

/* Report held events, in start order, whose fit window has run by this frame;
 * with force, the oldest one regardless (the queue is full) */
static void release_pending(bcd_time_detector_t *td, uint64_t frame, bool force) {
    while (td->pending_count > 0) {
        bcd_time_pending_t *p = &td->pending[td->pending_head];
        if (!force && p->release_frame > frame) break;
        if (force) p->fit = false;   /* its window has not run */
        report_event(td, p);
        td->pending_head = (td->pending_head + 1) % BCD_TIME_PENDING_MAX;
        td->pending_count--;
        force = false;
    }
}

float bcd_time_calculate_bucket_energy(bcd_time_detector_t *td) {
    return fft_processor_get_bucket_energy(td->fft, BCD_TIME_TARGET_FREQ_HZ, BCD_TIME_BANDWIDTH_HZ);
}

void bcd_time_run_state_machine(bcd_time_detector_t *td) {
    float energy = td->current_energy;
    uint64_t frame = td->frame_count;

    /* the step fit's history (BCD_TIME_FIT_*) */
    td->log_energy[frame % BCD_TIME_HIST_FRAMES] = logf(energy > 1e-9f ? energy : 1e-9f);

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

    /* Drop a pulse level and low level that no valid pulse has refreshed (see
     * BCD_TIME_LEVEL_HOLD_MS) */
    if ((td->pulse_level > 0.0f || td->low_level > 0.0f) &&
        (frame - td->last_valid_frame) * FRAME_DURATION_MS > BCD_TIME_LEVEL_HOLD_MS) {
        td->pulse_level = 0.0f;
        td->low_level = 0.0f;
    }

    /* An idle frame starts an event above threshold_high, half the pulse level and
     * BCD_TIME_START_LOW_RATIO times the low level, whichever is highest */
    float start_threshold = td->threshold_high;
    if (BCD_TIME_EDGE_FRACTION * td->pulse_level > start_threshold) {
        start_threshold = BCD_TIME_EDGE_FRACTION * td->pulse_level;
    }
    if (BCD_TIME_START_LOW_RATIO * td->low_level > start_threshold) {
        start_threshold = BCD_TIME_START_LOW_RATIO * td->low_level;
    }
    td->start_threshold = start_threshold;

    /* State machine */
    switch (td->state) {
        case STATE_IDLE: {
            if (energy > start_threshold) {
                td->state = STATE_IN_PULSE;
                td->pulse_start_frame = frame;
                td->pulse_peak_energy = energy;
                td->pulse_duration_frames = 1;
                td->consecutive_low_frames = 0;
                td->pulse_energy_sum = energy;
                td->pulse_energy_frames = 1;
            }
            break;
        }

        case STATE_IN_PULSE:
            td->pulse_duration_frames++;
            if (energy > td->pulse_peak_energy) {
                td->pulse_peak_energy = energy;
            }

            /* The pulse ends at half its own amplitude (BCD_TIME_EDGE_FRACTION of the
             * mean of its frames so far), or at threshold_low if that is higher.
             * Require consecutive low frames before ending it. */
            float end_threshold = BCD_TIME_EDGE_FRACTION *
                                  (td->pulse_energy_sum / td->pulse_energy_frames);
            if (end_threshold < td->threshold_low) end_threshold = td->threshold_low;
            if (energy < end_threshold) {
                td->consecutive_low_frames++;
            } else {
                td->consecutive_low_frames = 0;  /* Reset if signal returns */
                td->pulse_energy_sum += energy;
                td->pulse_energy_frames++;
            }

            if (td->consecutive_low_frames >= MIN_LOW_FRAMES) {
                /* Pulse ended - check validity */
                float duration_ms = td->pulse_duration_frames * FRAME_DURATION_MS;
                float snr_db = 10.0f * log10f(td->pulse_peak_energy / td->noise_floor);

                if (duration_ms >= BCD_TIME_PULSE_MIN_MS &&
                    duration_ms <= BCD_TIME_PULSE_MAX_MS) {
                    /* Valid pulse! */
                    td->pulses_detected++;
                    td->last_pulse_frame = td->pulse_start_frame;

                    /* Track the pulse level for the next start threshold */
                    float level = td->pulse_energy_sum / td->pulse_energy_frames;
                    td->pulse_level = (td->pulse_level > 0.0f)
                        ? td->pulse_level + BCD_TIME_LEVEL_ALPHA * (level - td->pulse_level)
                        : level;
                    td->last_valid_frame = frame;

                    /* Hold it for the step fit (BCD_TIME_FIT_*), in start order */
                    if (td->pending_count == BCD_TIME_PENDING_MAX) {
                        release_pending(td, frame, true);
                    }
                    uint64_t sf = td->pulse_start_frame;
                    bool fit = duration_ms <= BCD_TIME_FIT_EDGE_MAX_MS &&
                               sf >= (uint64_t)MS_TO_FRAMES(BCD_TIME_FIT_PRE_MS) + 1;
                    uint64_t release = frame;
                    if (fit && sf + (uint64_t)MS_TO_FRAMES(BCD_TIME_FIT_SPAN_MS) > release) {
                        release = sf + (uint64_t)MS_TO_FRAMES(BCD_TIME_FIT_SPAN_MS);
                    }
                    int k = (td->pending_head + td->pending_count) % BCD_TIME_PENDING_MAX;
                    td->pending[k] = (bcd_time_pending_t){
                        .start_frame = sf,
                        .release_frame = release,
                        .duration_ms = duration_ms,
                        .fit = fit,
                        .peak_energy = td->pulse_peak_energy,
                        .noise_floor = td->noise_floor,
                        .snr_db = snr_db,
                        .pulse_num = td->pulses_detected
                    };
                    td->pending_count++;
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

                if (duration_ms < BCD_TIME_PULSE_MIN_MS) {
                    /* Too short to be a pulse: no dead time after it. The tick at the
                     * second leaks a segment like this just before the 100 Hz pulse
                     * rises (+30 ms), and a cooldown here would hide that rise. */
                    td->state = STATE_IDLE;
                } else {
                    td->state = STATE_COOLDOWN;
                    td->cooldown_frames = MS_TO_FRAMES(BCD_TIME_COOLDOWN_MS);
                    td->cooldown_energy_frames = 0;
                }
            }
            break;

        case STATE_COOLDOWN:
            /* after an event's end: the low level (BCD_TIME_START_LOW_RATIO) */
            if (td->cooldown_energy_frames < BCD_TIME_COOLDOWN_FRAMES_MAX) {
                td->cooldown_energy[td->cooldown_energy_frames++] = energy;
            }
            if (--td->cooldown_frames <= 0) {
                td->state = STATE_IDLE;
                /* the second half: the rest of a pulse cut short comes first */
                int half = td->cooldown_energy_frames / 2;
                td->low_level = median_of(td->cooldown_energy + half,
                                          td->cooldown_energy_frames - half);
            }
            break;
    }

    /* Report held events whose fit window has run */
    release_pending(td, frame, false);
}
