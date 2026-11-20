/**
 * @file sync_detector.c
 * @brief WWV unified sync detector implementation
 *
 * Fuses evidence from multiple sources (ticks, markers, P-markers, tick-holes)
 * to provide authoritative frame timing with confidence tracking and recovery.
 */

#include "sync_detector.h"
#include "wwv_clock.h"
#include "version.h"
#include "telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Configuration - from spec
 *============================================================================*/

/* Legacy correlation (backward compat) */
#define CORRELATION_WINDOW_MS   1500.0f
#define MARKER_INTERVAL_MIN_MS  55000.0f
#define MARKER_INTERVAL_MAX_MS  65000.0f
#define MARKER_NOMINAL_MS       60000.0f
#define PENDING_TIMEOUT_MS      3000.0f
#define SYNC_FLASH_FRAMES       30

/* Timing thresholds */
#define TICK_INTERVAL_MIN_MS         950.0f
#define TICK_INTERVAL_MAX_MS         1050.0f
#define TICK_HOLE_MIN_GAP_MS         1700.0f
#define TICK_HOLE_MAX_GAP_MS         2200.0f
#define TICK_DOUBLE_MAX_GAP_MS       100.0f

/* Holdover. Once LOCKED, the minute phase is held on the sample clock through
 * minutes whose marker is not detected: a missing marker is missing evidence,
 * not contradicting evidence. On the 90-minute live RSP2 run, 44 of 90 markers
 * were missed (at most 4 in a row) and the 46 detected were all within -45..+61
 * ms of :00, while the TCXO drifts about 1 ppm (6 ms in 90 minutes). A marker
 * off the held phase is handled by confirm_marker()'s re-anchor rule, not by
 * dropping lock. After SYNC_HOLDOVER_MS with no marker, frames stop (the
 * signal is taken as gone, so the decoder is not fed noise minutes beyond a
 * third of its 60-minute history) and sync enters RECOVERING, which keeps the
 * phase: one marker on it within marker_tolerance_ms re-locks. After
 * SYNC_RETENTION_WINDOW_MS from the loss, sync starts over. At 10 ppm the held
 * phase moves 48 ms in the 80 minutes. */
#define SYNC_HOLDOVER_MS             (20.0f * 60000.0f)
#define SYNC_RETENTION_WINDOW_MS     (60.0f * 60000.0f)

/* Marker candidates (sync_detector_marker_candidate). A candidate is a run a
 * detector rejected as a marker but that may be one: on the tick path, a marker
 * whose start was hidden by the cooldown after a noise run just before :00 (live,
 * 99 of 268 minutes; it ends on time, +812..+826 ms on the captures); in the
 * marker detector, a window run too short to count. It confirms a minute phase
 * already held from a marker when either of its leading-edge readings lands
 * within CANDIDATE_TOLERANCE_MS of a whole number of minutes from the anchor. It
 * never creates or moves a phase: the confirmed time is the held phase, not the
 * candidate's. The tolerance covers a marker-detector anchor (-66..+109 ms of
 * :00 on the captures) against a tick-path end (+-10 ms). Candidates off :00
 * come at 1.1 a minute on the captures (45 in 42 minutes, the :52.5 voice among
 * them), so with two readings a random one lands within the tolerance in about
 * 1 minute in 90. It then confirms the phase held, which matters only if that
 * phase is wrong, and a wrong phase still needs a second such coincidence (or a
 * real marker on it) to lock. */
#define CANDIDATE_TOLERANCE_MS       150.0f

/* Second-phase tracking (sync_detector_second_edge). The held phase counts
 * samples, so samples lost upstream move it: on live run90g (15 MHz) the stream
 * lost about 3 ms/s (up to 5 ms/s) for 30 minutes, 5.3 s in all, and the held
 * phase was 0.66 s, 1.39 s and 1.24 s late when the next markers re-anchored it.
 * A window opens WINDOW_LEAD_MS (100 ms) before its second and a pulse starts
 * about 40 ms after it, so a phase about 140 ms late already puts every pulse in
 * the previous window. While LOCKED, the edges that mark whole seconds (ticks,
 * and the leading edges of BCD time-detector pulses) keep the sub-second phase:
 *   - each edge's offset from its predicted second (the held phase plus the
 *     edge's own lead, PHASE_LEAD_*) is taken modulo 1 s;
 *   - only edges within PHASE_GATE_MS of the prediction count, over the last
 *     PHASE_WINDOW_MS. A pulse's width (its symbol) is never used;
 *   - the estimate is the median of the in-gate ticks when there are at least
 *     PHASE_MIN_EDGES of them (ticks are sharp on a strong signal: +-5 ms), else
 *     of all in-gate edges (BCD starts are broad, but on the weak 15 MHz run they
 *     are 5-10 times as many as true ticks). Beyond the dead band
 *     (PHASE_DEADBAND_MS for ticks; PHASE_DEADBAND_BTIME_MS when BCD starts set
 *     it, whose lead differs by band: +17..+34 ms on the 5/10 MHz runs, later on
 *     15 MHz), the phase moves by PHASE_GAIN of it, at most once per
 *     PHASE_EVAL_MS. A slew is at most PHASE_GAIN x PHASE_GATE_MS.
 * Ticks give the phase modulo 1 s only, so the tracker never starts a lock,
 * never chooses the minute and cannot see whole seconds: a loss of n s + d reads
 * as d. Whole seconds come from markers (confirm_marker()'s +-5 s re-anchor) and
 * from a candidate that lands a whole number of seconds off the held phase.
 * A jump the slew cannot follow shows as a tick cluster outside
 * PHASE_JUMP_GATE_MS: over PHASE_JUMP_WINDOW_MS, at least PHASE_JUMP_MIN_TICKS
 * ticks within PHASE_JUMP_RADIUS_MS of each other, PHASE_JUMP_BG_RATIO times the
 * count uniform false ticks would put there, and more than twice the ticks near
 * the held phase. Lost samples only make edges early: an early cluster within
 * PHASE_SNAP_SUPPORT_MS of the last edge or marker support is the loss since
 * then, and the phase moves to it (a break). Any other cluster (late, or after
 * a long gap) is ambiguous: sync drops to RECOVERING with the phase marked
 * unknown, and the next marker within +-5 s of the minute re-locks on its own
 * time. On run90g seg2 the slews follow 3 ms/s only in part; most of the 5.3 s
 * is taken back by the snaps and the marker re-anchors. Measured by replaying
 * the chain on the live logs (run90, run90c, run90d, run90f, run90g seg2): no
 * jump on the runs with no loss. */
#define PHASE_WINDOW_MS              12000.0f
#define PHASE_EVAL_MS                1000.0f
#define PHASE_GATE_MS                40.0f
#define PHASE_MIN_EDGES              4
#define PHASE_DEADBAND_MS            8.0f
#define PHASE_DEADBAND_BTIME_MS      15.0f
#define PHASE_GAIN                   0.5f
#define PHASE_LEAD_TICK_MS           15.0f    /* tick detector, from the marker lead */
#define PHASE_LEAD_BTIME_MS          30.0f    /* BCD time detector leading edge */
#define PHASE_JUMP_WINDOW_MS         30000.0f
#define PHASE_JUMP_GATE_MS           80.0f
#define PHASE_JUMP_RADIUS_MS         15.0f
#define PHASE_JUMP_MIN_TICKS         6
#define PHASE_JUMP_BG_RATIO          4.0f
#define PHASE_SNAP_SUPPORT_MS        60000.0f
/* A phase move at least this large is a break: windows and frames that span it
 * are discarded (sync_detector_get_phase_epoch). Slews stay below it. */
#define PHASE_BREAK_MS               100.0f
/* A candidate within this of a whole number of seconds (1..5) off the held
 * phase says the held phase is a whole number of seconds off */
#define CANDIDATE_SECOND_TOL_MS      60.0f
#define PHASE_EDGES_MAX              256

/* Evidence weights */
#define WEIGHT_TICK                  0.05f
#define WEIGHT_MARKER                0.40f
#define WEIGHT_P_MARKER              0.15f
#define WEIGHT_TICK_HOLE             0.20f
#define WEIGHT_COMBINED_HOLE_MARKER  0.50f

/* Confidence thresholds */
#define CONFIDENCE_LOCKED_THRESHOLD  0.70f
#define CONFIDENCE_MIN_RETAIN        0.05f
#define CONFIDENCE_TENTATIVE_INIT    0.30f
/* Changed from 0.995f (95% decay in 60s) to 0.9999f (6% decay in 60s) - allows accumulation */
#define CONFIDENCE_DECAY_NORMAL      0.9999f
#define CONFIDENCE_DECAY_RECOVERING  0.980f

/* Validation tolerances */
#define TICK_PHASE_TOLERANCE_MS      100.0f
#define MARKER_TOLERANCE_MS          500.0f
#define P_MARKER_TOLERANCE_MS        200.0f
#define LEAP_SECOND_EXTRA_MS         1000.0f

/* Debounce */
#define MIN_TICKS_FOR_HOLE           20
#define SIGNAL_WEAK_DEBOUNCE         3

/*============================================================================
 * Internal State
 *============================================================================*/

typedef struct {
    int consecutive_tick_count;
    float last_tick_ms;
    float prev_hole_ms;
    float last_hole_ms;
    int hole_count;
} tick_gap_tracker_t;

typedef struct {
    float retained_anchor_ms;
    float signal_lost_ms;
    float recovery_start_ms;
    bool has_retained_state;
    bool recovery_tick_seen;
    bool recovery_marker_seen;
    bool recovery_p_marker_seen;
    bool phase_unknown;             /* entered on a jump: whole seconds unknown */
} recovery_state_t;

typedef struct {
    float t_ms;
    uint8_t kind;                   /* sync_edge_kind_t */
} phase_edge_t;

struct sync_detector {
    /* Legacy pending events (backward compat) */
    float pending_tick_ms;
    float pending_tick_duration_ms;
    float pending_tick_corr_ratio;
    bool tick_pending;
    float pending_marker_ms;
    float pending_marker_energy;
    float pending_marker_duration_ms;
    bool marker_pending;

    /* Confirmed markers */
    float last_confirmed_ms;
    float prev_confirmed_ms;
    float last_rejected_ms;         /* Most recent rejected marker, 0 = none */
    bool last_confirm_weak;         /* The last confirmation was a candidate's */
    int confirmed_count;
    int good_intervals;

    /* Enhanced state */
    sync_state_t state;
    float confidence;
    uint32_t evidence_mask;
    float minute_anchor_ms;         /* Authoritative minute boundary */

    /* Tick gap tracking */
    tick_gap_tracker_t tick_gap;

    /* Recovery state */
    recovery_state_t recovery;

    /* Second-phase tracking */
    phase_edge_t edges[PHASE_EDGES_MAX];
    int edge_head, edge_count;
    float last_phase_eval_ms;
    int phase_epoch;
    float phase_support_ms;         /* last time the held phase had edge or marker support */
    sync_phase_stats_t phase_stats;

    /* Signal loss detection */
    int signal_weak_count;
    bool expecting_marker_soon;
    float expected_marker_ms;

    /* Optional integrations */
    wwv_clock_t *wwv_clock;
    bool leap_second_pending;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float weight_tick;                      /* Tick evidence weight (0.01-0.2, default 0.05) */
    float weight_marker;                    /* Marker evidence weight (0.1-0.6, default 0.40) */
    float weight_p_marker;                  /* P-marker evidence weight (0.05-0.3, default 0.15) */
    float weight_tick_hole;                 /* Tick hole evidence weight (0.05-0.4, default 0.20) */
    float weight_combined_hole_marker;      /* Combined hole+marker weight (0.2-0.8, default 0.50) */
    float confidence_locked_threshold;      /* Threshold to reach LOCKED (0.5-0.9, default 0.70) */
    float confidence_min_retain;            /* Minimum to keep state (0.01-0.2, default 0.05) */
    float confidence_tentative_init;        /* Initial tentative confidence (0.1-0.5, default 0.30) */
    float confidence_decay_normal;          /* Normal decay rate (0.99-0.9999, default 0.9999) */
    float confidence_decay_recovering;      /* Recovery mode decay (0.90-0.99, default 0.980) */
    float tick_phase_tolerance_ms;          /* Tick timing tolerance (50.0-200.0, default 100.0) */
    float marker_tolerance_ms;              /* Marker timing tolerance (200.0-800.0, default 500.0) */
    float p_marker_tolerance_ms;            /* P-marker timing tolerance (100.0-400.0, default 200.0) */

    /* Callbacks */
    sync_state_callback_fn state_callback;
    void *state_callback_user_data;

    /* UI feedback */
    int flash_frames_remaining;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Internal Functions
 *============================================================================*/

/* Forward declarations */
static void transition_state(sync_detector_t *sd, sync_state_t new_state);
static void apply_evidence(sync_detector_t *sd, uint32_t evidence_type, float weight);
static float get_evidence_weight(sync_detector_t *sd, uint32_t evidence_type);
static void sync_detector_hole_detected(sync_detector_t *sd, float hole_timestamp_ms);
static void sync_detector_full_reset(sync_detector_t *sd);

static void get_wall_time_str(sync_detector_t *sd, float timestamp_ms, char *buf, size_t buflen) {
    time_t event_time = sd->start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

static void log_confirmed_marker(sync_detector_t *sd, float timestamp_ms,
                                  float interval_ms, float delta_ms,
                                  const char *source) {
    if (!sd->csv_file) return;

    char time_str[16];
    get_wall_time_str(sd, timestamp_ms, time_str, sizeof(time_str));

    fprintf(sd->csv_file, "%s,%.1f,%d,%s,%.1f,%.0f,%.1f,%.1f\n",
            time_str,
            timestamp_ms,
            sd->confirmed_count,
            sync_state_name(sd->state),
            interval_ms / 1000.0f,
            delta_ms,
            sd->pending_tick_duration_ms,
            sd->pending_marker_duration_ms);
    fflush(sd->csv_file);

    /* UDP telemetry broadcast - expanded format */
    telem_sendf(TELEM_SYNC, "%s,%.1f,%d,%s,%d,%.1f,%.0f,%.1f,%.1f,%.1f",
                time_str, timestamp_ms, sd->confirmed_count,
                sync_state_name(sd->state), sd->good_intervals,
                interval_ms / 1000.0f, delta_ms,
                sd->pending_tick_duration_ms, sd->pending_marker_duration_ms,
                sd->last_confirmed_ms);
}

/* Sub-minute part of a phase change, in (-30 s, +30 s] */
static float minute_wrap(float d) {
    return d - roundf(d / MARKER_NOMINAL_MS) * MARKER_NOMINAL_MS;
}

/* A move of the held phase by at least PHASE_BREAK_MS starts a new phase epoch */
static void note_phase_move(sync_detector_t *sd, float old_ms, float new_ms, const char *why) {
    if (old_ms <= 0.0f) return;
    float d = minute_wrap(new_ms - old_ms);
    if (fabsf(d) < PHASE_BREAK_MS) return;
    sd->phase_epoch++;
    sd->phase_stats.breaks++;
    printf("[SYNC] Phase break #%d (%s): held phase moved %+.0f ms\n", sd->phase_epoch, why, d);
}

static void confirm_marker(sync_detector_t *sd, float marker_time, float delta_ms, const char *source) {
    /* The marker of a minute a candidate already confirmed: it is measured, the
     * candidate's time was the held phase, so take its time; count it once. */
    if (sd->last_confirm_weak && sd->last_confirmed_ms > 0 &&
        fabsf(marker_time - sd->last_confirmed_ms) < CORRELATION_WINDOW_MS) {
        printf("[SYNC] Marker (%s) of the minute a candidate confirmed: anchor %.1fms -> %.1fms\n",
               source, sd->last_confirmed_ms, marker_time);
        note_phase_move(sd, sd->last_confirmed_ms, marker_time, "marker of a candidate's minute");
        sd->last_confirmed_ms = marker_time;
        sd->minute_anchor_ms = marker_time;
        sd->last_confirm_weak = false;
        sd->tick_pending = false;
        sd->marker_pending = false;
        return;
    }
    sd->last_confirm_weak = false;

    /* Calculate interval from previous marker */
    float interval_ms = (sd->last_confirmed_ms > 0) ?
        (marker_time - sd->last_confirmed_ms) : 0.0f;

    /* Check if interval is a valid multiple of ~60 seconds (allows for missed markers) */
    bool good_interval = (sd->last_confirmed_ms == 0);
    if (!good_interval && interval_ms >= MARKER_INTERVAL_MIN_MS) {
        /* Find how many 60-second periods fit */
        int periods = (int)((interval_ms + MARKER_NOMINAL_MS/2) / MARKER_NOMINAL_MS);
        float expected_ms = periods * MARKER_NOMINAL_MS;
        float error_ms = fabsf(interval_ms - expected_ms);
        /* Accept if within ±5 seconds of a multiple of 60s */
        good_interval = (error_ms <= 5000.0f);
    }

    /* Re-anchor on positive evidence only. A wrong anchor (a false first marker)
     * would otherwise reject every real marker for good, because each is judged
     * against it. Two consecutive rejected markers one minute apart outvote it.
     * A lone false marker never re-anchors: any accepted marker clears the
     * rejected candidate. */
    if (!good_interval && sd->last_rejected_ms > 0) {
        float since_rejected = marker_time - sd->last_rejected_ms;
        if (since_rejected >= MARKER_INTERVAL_MIN_MS &&
            since_rejected <= MARKER_INTERVAL_MAX_MS) {
            printf("[SYNC] Re-anchor: markers at %.1fms and %.1fms are %.1fs apart; "
                   "dropping anchor %.1fms\n", sd->last_rejected_ms, marker_time,
                   since_rejected / 1000.0f, sd->last_confirmed_ms);
            note_phase_move(sd, sd->last_confirmed_ms, sd->last_rejected_ms, "re-anchor");
            sd->last_confirmed_ms = sd->last_rejected_ms;
            sd->good_intervals = 0;
            interval_ms = since_rejected;
            good_interval = true;
        }
    }

    if (good_interval) {
        sd->last_rejected_ms = 0.0f;
        /* Update confirmed marker history */
        sd->prev_confirmed_ms = sd->last_confirmed_ms;
        note_phase_move(sd, sd->last_confirmed_ms, marker_time, source);
        sd->last_confirmed_ms = marker_time;
        sd->minute_anchor_ms = marker_time;  /* Set authoritative anchor */
        sd->phase_support_ms = marker_time;
        sd->confirmed_count++;

        /* Track good intervals for lock confidence */
        if (interval_ms >= MARKER_INTERVAL_MIN_MS &&
            interval_ms <= MARKER_INTERVAL_MAX_MS) {
            sd->good_intervals++;
        }

        /* Apply marker evidence */
        float weight = get_evidence_weight(sd, EVIDENCE_MARKER);

        /* Check if marker confirms :59 tick hole */
        if (sd->expecting_marker_soon) {
            float delta_from_expected = fabsf(marker_time - sd->expected_marker_ms);
            if (delta_from_expected < 200.0f) {
                printf("[SYNC] Marker confirms :59 tick hole - high confidence\n");
                weight = WEIGHT_COMBINED_HOLE_MARKER;
            }
            sd->expecting_marker_soon = false;
        }

        apply_evidence(sd, EVIDENCE_MARKER, weight);

        /* Legacy state transitions */
        if (sd->state == SYNC_ACQUIRING) {
            transition_state(sd, SYNC_TENTATIVE);
        }

        /* In recovery, a marker on the retained minute phase validates it. After
         * a jump the retained phase is off by an unknown amount: a marker within
         * the +-5 s interval test (above) is the new phase. */
        if (sd->state == SYNC_RECOVERING && sd->recovery.has_retained_state) {
            float off = minute_wrap(marker_time - sd->recovery.retained_anchor_ms);
            if (fabsf(off) <= sd->marker_tolerance_ms || sd->recovery.phase_unknown)
                sd->recovery.recovery_marker_seen = true;
        }

        /* Flash for UI */
        sd->flash_frames_remaining = SYNC_FLASH_FRAMES;

        /* Log to CSV */
        log_confirmed_marker(sd, marker_time, interval_ms, delta_ms, source);

        /* Console output */
        printf("[SYNC] *** CONFIRMED MARKER #%d *** src=%s delta=%.0fms interval=%.1fs state=%s conf=%.2f\n",
               sd->confirmed_count, source, delta_ms, interval_ms / 1000.0f,
               sync_state_name(sd->state), sd->confidence);
    } else {
        printf("[SYNC] Marker rejected (%s): interval=%.1fs (out of range)\n",
               source, interval_ms / 1000.0f);
        sd->last_rejected_ms = marker_time;
    }

    /* Clear both pending events */
    sd->tick_pending = false;
    sd->marker_pending = false;
}

static void try_correlate(sync_detector_t *sd, float current_ms) {
    /* Accept EITHER detector firing - no longer require both */

    /* If both are pending, check correlation and use the better source */
    if (sd->tick_pending && sd->marker_pending) {
        float delta = fabsf(sd->pending_marker_ms - sd->pending_tick_ms);
        if (delta < CORRELATION_WINDOW_MS) {
            /* Both detectors agree - use tick detector timestamp (more precise) */
            confirm_marker(sd, sd->pending_tick_ms, delta, "BOTH");
            return;
        }
        /* If they don't correlate, prefer the earlier one */
        if (sd->pending_tick_ms < sd->pending_marker_ms) {
            confirm_marker(sd, sd->pending_tick_ms, 0.0f, "TICK");
        } else {
            confirm_marker(sd, sd->pending_marker_ms, 0.0f, "MARK");
        }
        return;
    }

    /* Single detector fired - no correlation partner available yet */
    /* The timeout handler will confirm if partner doesn't arrive */
}

static void check_timeout(sync_detector_t *sd, float current_ms) {
    /* If a single detector fired and partner didn't arrive, confirm from single source */
    if (sd->tick_pending && !sd->marker_pending &&
        (current_ms - sd->pending_tick_ms) > PENDING_TIMEOUT_MS) {
        confirm_marker(sd, sd->pending_tick_ms, 0.0f, "TICK");
        return;
    }
    if (sd->marker_pending && !sd->tick_pending &&
        (current_ms - sd->pending_marker_ms) > PENDING_TIMEOUT_MS) {
        confirm_marker(sd, sd->pending_marker_ms, 0.0f, "MARK");
        return;
    }
}

/*============================================================================
 * Enhanced Sync Functions
 *============================================================================*/

static float get_evidence_weight(sync_detector_t *sd, uint32_t evidence_type) {
    float base_weight;

    switch (evidence_type) {
        case EVIDENCE_TICK:       base_weight = sd->weight_tick; break;
        case EVIDENCE_MARKER:     base_weight = sd->weight_marker; break;
        case EVIDENCE_P_MARKER:   base_weight = sd->weight_p_marker; break;
        case EVIDENCE_TICK_HOLE:  base_weight = sd->weight_tick_hole; break;
        default:                  base_weight = 0.10f; break;
    }

    /* Schedule-aware discount if wwv_clock available */
    if (sd->wwv_clock && wwv_clock_is_special_minute(sd->wwv_clock)) {
        base_weight *= 0.5f;
    }

    return base_weight;
}

static void apply_evidence(sync_detector_t *sd, uint32_t evidence_type, float weight) {
    sd->evidence_mask |= evidence_type;

    /* Boost confidence asymptotically toward 1.0 */
    sd->confidence += weight * (1.0f - sd->confidence);
    if (sd->confidence > 1.0f) sd->confidence = 1.0f;

    /* State transitions based on confidence */
    if (sd->state == SYNC_TENTATIVE && sd->confidence >= sd->confidence_locked_threshold) {
        transition_state(sd, SYNC_LOCKED);
    }
}

static void transition_state(sync_detector_t *sd, sync_state_t new_state) {
    if (sd->state == new_state) return;

    sync_state_t old_state = sd->state;
    sd->state = new_state;

    /* State-specific initialization */
    switch (new_state) {
        case SYNC_ACQUIRING:
            sd->confidence = 0.0f;
            break;
        case SYNC_TENTATIVE:
            if (sd->confidence < CONFIDENCE_TENTATIVE_INIT) {
                sd->confidence = CONFIDENCE_TENTATIVE_INIT;
            }
            break;
        case SYNC_LOCKED:
            /* Confidence already above threshold */
            break;
        case SYNC_RECOVERING:
            /* Confidence retained; the caller set the recovery times */
            break;
    }

    printf("[SYNC] State transition: %s → %s (conf=%.2f)\n",
           sync_state_name(old_state), sync_state_name(new_state), sd->confidence);

    /* UDP telemetry */
    telem_sendf(TELEM_SYNC, "STATE,%s,%s,%.2f",
                sync_state_name(old_state), sync_state_name(new_state), sd->confidence);

    /* User callback */
    if (sd->state_callback) {
        sd->state_callback(old_state, new_state, sd->confidence, sd->state_callback_user_data);
    }
}

static void sync_detector_hole_detected(sync_detector_t *sd, float hole_timestamp_ms) {
    int probable_second = -1;

    /* Determine position if locked */
    if (sd->state == SYNC_LOCKED && sd->minute_anchor_ms > 0) {
        float since_anchor = hole_timestamp_ms - sd->minute_anchor_ms;
        while (since_anchor < 0) since_anchor += 60000.0f;
        while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

        int position = (int)(since_anchor / 1000.0f + 0.5f);

        if (abs(position - 29) <= 1) {
            probable_second = 29;
        } else if (abs(position - 59) <= 1 || position <= 1) {
            probable_second = 59;
            sd->expecting_marker_soon = true;
            sd->expected_marker_ms = hole_timestamp_ms + 1000.0f;
        }
    }

    float weight = get_evidence_weight(sd, EVIDENCE_TICK_HOLE);

    if (probable_second >= 0) {
        printf("[SYNC] Tick hole at second %d (%.0fms)\n", probable_second, hole_timestamp_ms);
        apply_evidence(sd, EVIDENCE_TICK_HOLE, weight);
    } else if (sd->state == SYNC_ACQUIRING) {
        /* During acquisition, look for double-hole pattern */
        sd->tick_gap.hole_count++;
        if (sd->tick_gap.hole_count >= 2 && sd->tick_gap.prev_hole_ms > 0) {
            float hole_interval = hole_timestamp_ms - sd->tick_gap.prev_hole_ms;
            if (fabsf(hole_interval - 30000.0f) < 500.0f) {
                printf("[SYNC] Double tick-hole pattern confirmed (%.1fs apart)\n",
                       hole_interval / 1000.0f);
                apply_evidence(sd, EVIDENCE_TICK_HOLE, weight * 1.5f);
            }
        }
        sd->tick_gap.prev_hole_ms = sd->tick_gap.last_hole_ms;
        sd->tick_gap.last_hole_ms = hole_timestamp_ms;
    }
}

static void sync_detector_full_reset(sync_detector_t *sd) {
    printf("[SYNC] Full reset - clearing all state\n");

    memset(&sd->tick_gap, 0, sizeof(tick_gap_tracker_t));
    memset(&sd->recovery, 0, sizeof(recovery_state_t));
    sd->edge_count = 0;

    sd->confidence = 0.0f;
    sd->evidence_mask = 0;
    sd->signal_weak_count = 0;
    sd->expecting_marker_soon = false;

    transition_state(sd, SYNC_ACQUIRING);
}

/* The held phase is off by an unknown number of whole seconds: frames stop
 * (RECOVERING) and the next marker within +-5 s of the minute re-locks. */
static void enter_phase_unknown(sync_detector_t *sd, float now_ms, const char *why) {
    if (sd->state != SYNC_LOCKED) return;
    printf("[SYNC] Held phase lost (%s) - whole seconds unknown, waiting for a marker\n", why);
    sd->phase_epoch++;
    sd->recovery.retained_anchor_ms = sd->minute_anchor_ms;
    sd->recovery.signal_lost_ms = now_ms;
    sd->recovery.recovery_start_ms = now_ms;
    sd->recovery.has_retained_state = true;
    sd->recovery.recovery_tick_seen = false;
    sd->recovery.recovery_marker_seen = false;
    sd->recovery.recovery_p_marker_seen = false;
    sd->recovery.phase_unknown = true;
    sd->last_confirm_weak = false;
    sd->edge_count = 0;
    transition_state(sd, SYNC_RECOVERING);
}

/*============================================================================
 * Public API
 *============================================================================*/

sync_detector_t *sync_detector_create(const char *csv_path) {
    sync_detector_t *sd = (sync_detector_t *)calloc(1, sizeof(sync_detector_t));
    if (!sd) return NULL;

    sd->state = SYNC_ACQUIRING;
    sd->start_time = time(NULL);

    /* Initialize tunable parameters to defaults */
    sd->weight_tick = WEIGHT_TICK;                                      /* 0.05 */
    sd->weight_marker = WEIGHT_MARKER;                                  /* 0.40 */
    sd->weight_p_marker = WEIGHT_P_MARKER;                              /* 0.15 */
    sd->weight_tick_hole = WEIGHT_TICK_HOLE;                            /* 0.20 */
    sd->weight_combined_hole_marker = WEIGHT_COMBINED_HOLE_MARKER;      /* 0.50 */
    sd->confidence_locked_threshold = CONFIDENCE_LOCKED_THRESHOLD;      /* 0.70 */
    sd->confidence_min_retain = CONFIDENCE_MIN_RETAIN;                  /* 0.05 */
    sd->confidence_tentative_init = CONFIDENCE_TENTATIVE_INIT;          /* 0.30 */
    sd->confidence_decay_normal = CONFIDENCE_DECAY_NORMAL;              /* 0.9999 */
    sd->confidence_decay_recovering = CONFIDENCE_DECAY_RECOVERING;      /* 0.980 */
    sd->tick_phase_tolerance_ms = TICK_PHASE_TOLERANCE_MS;              /* 100.0 */
    sd->marker_tolerance_ms = MARKER_TOLERANCE_MS;                      /* 500.0 */
    sd->p_marker_tolerance_ms = P_MARKER_TOLERANCE_MS;                  /* 200.0 */

    /* Open CSV file */
    if (csv_path) {
        sd->csv_file = fopen(csv_path, "w");
        if (sd->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

            fprintf(sd->csv_file, "# Phoenix SDR WWV Sync Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(sd->csv_file, "# Started: %s\n", time_str);
            fprintf(sd->csv_file, "time,timestamp_ms,marker_num,state,interval_sec,delta_ms,tick_dur_ms,marker_dur_ms\n");
            fflush(sd->csv_file);
        }
    }

    printf("[SYNC] Detector created, state=ACQUIRING\n");

    return sd;
}

void sync_detector_destroy(sync_detector_t *sd) {
    if (!sd) return;

    if (sd->csv_file) {
        fclose(sd->csv_file);
    }

    free(sd);
}

void sync_detector_tick_marker(sync_detector_t *sd, float timestamp_ms,
                                float duration_ms, float corr_ratio) {
    if (!sd) return;

    /* Check for timeout on previous pending events */
    check_timeout(sd, timestamp_ms);

    /* Store pending tick marker event */
    sd->pending_tick_ms = timestamp_ms;
    sd->pending_tick_duration_ms = duration_ms;
    sd->pending_tick_corr_ratio = corr_ratio;
    sd->tick_pending = true;

    printf("[SYNC] Tick marker received: %.1fms dur=%.0fms\n", timestamp_ms, duration_ms);

    /* Try to correlate with pending marker event */
    try_correlate(sd, timestamp_ms);
}

void sync_detector_marker_event(sync_detector_t *sd, float timestamp_ms,
                                 float accum_energy, float duration_ms) {
    if (!sd) return;

    /* Check for timeout on previous pending events */
    check_timeout(sd, timestamp_ms);

    /* Store pending marker event */
    sd->pending_marker_ms = timestamp_ms;
    sd->pending_marker_energy = accum_energy;
    sd->pending_marker_duration_ms = duration_ms;
    sd->marker_pending = true;

    printf("[SYNC] Marker event received: %.1fms energy=%.0f dur=%.0fms\n",
           timestamp_ms, accum_energy, duration_ms);

    /* Try to correlate with pending tick marker */
    try_correlate(sd, timestamp_ms);
}

void sync_detector_marker_candidate(sync_detector_t *sd, float lead_a_ms, float lead_b_ms,
                                    const char *source) {
    if (!sd) return;
    /* No phase to test it against: a candidate never starts one */
    if (sd->state == SYNC_ACQUIRING || sd->minute_anchor_ms <= 0.0f) return;
    /* After a jump the held phase is not known to the second */
    if (sd->state == SYNC_RECOVERING && sd->recovery.phase_unknown) return;
    /* A detector already reported this minute's marker; it decides */
    if (sd->tick_pending || sd->marker_pending) return;

    const float leads[2] = { lead_a_ms, lead_b_ms };
    float best_off = 0.0f, predicted = 0.0f;
    bool on_phase = false;
    for (int i = 0; i < 2; i++) {
        float since = leads[i] - sd->minute_anchor_ms;
        float n = roundf(since / MARKER_NOMINAL_MS);
        float off = since - n * MARKER_NOMINAL_MS;
        if (n >= 1.0f && fabsf(off) <= CANDIDATE_TOLERANCE_MS &&
            (!on_phase || fabsf(off) < fabsf(best_off))) {
            on_phase = true;
            best_off = off;
            predicted = sd->minute_anchor_ms + n * MARKER_NOMINAL_MS;
        }
    }
    if (!on_phase) {
        printf("[SYNC] Candidate (%s) off the held phase: leads %.1fms / %.1fms\n",
               source, lead_a_ms, lead_b_ms);
        /* A whole number of seconds off: the held phase may be right modulo 1 s
         * and seconds off (a loss of n s + d that the tracker followed as d). It
         * cannot be told which is wrong, so the phase is not trusted. */
        if (sd->state == SYNC_LOCKED) {
            for (int i = 0; i < 2; i++) {
                float since = leads[i] - sd->minute_anchor_ms;
                if (roundf(since / MARKER_NOMINAL_MS) < 1.0f) continue;
                float off = minute_wrap(since);
                float k = roundf(off / 1000.0f);
                if (fabsf(k) >= 1.0f && fabsf(k) <= 5.0f &&
                    fabsf(off - k * 1000.0f) <= CANDIDATE_SECOND_TOL_MS) {
                    char why[64];
                    snprintf(why, sizeof why, "candidate %s %+.0f s off", source, k);
                    sd->phase_stats.candidate_seconds++;
                    enter_phase_unknown(sd, leads[i], why);
                    return;
                }
            }
        }
        return;
    }
    /* This minute is already confirmed */
    if (sd->last_confirmed_ms > 0.0f && fabsf(predicted - sd->last_confirmed_ms) < MARKER_INTERVAL_MIN_MS)
        return;

    printf("[SYNC] Candidate (%s) on the held phase, %+.0fms: confirms %.1fms\n",
           source, best_off, predicted);
    confirm_marker(sd, predicted, best_off, source);
    if (sd->last_confirmed_ms == predicted) sd->last_confirm_weak = true;
}

sync_state_t sync_detector_get_state(sync_detector_t *sd) {
    return sd ? sd->state : SYNC_ACQUIRING;
}

const char *sync_state_name(sync_state_t state) {
    switch (state) {
        case SYNC_ACQUIRING:  return "ACQUIRING";
        case SYNC_TENTATIVE:  return "TENTATIVE";
        case SYNC_LOCKED:     return "LOCKED";
        case SYNC_RECOVERING: return "RECOVERING";
        default:              return "UNKNOWN";
    }
}

float sync_detector_get_last_marker_ms(sync_detector_t *sd) {
    return sd ? sd->last_confirmed_ms : 0.0f;
}

int sync_detector_get_confirmed_count(sync_detector_t *sd) {
    return sd ? sd->confirmed_count : 0;
}

int sync_detector_get_flash_frames(sync_detector_t *sd) {
    return sd ? sd->flash_frames_remaining : 0;
}

void sync_detector_decrement_flash(sync_detector_t *sd) {
    if (sd && sd->flash_frames_remaining > 0) {
        sd->flash_frames_remaining--;
    }
}

int sync_detector_get_good_intervals(sync_detector_t *sd) {
    return sd ? sd->good_intervals : 0;
}

void sync_detector_broadcast_state(sync_detector_t *sd) {
    if (!sd) return;

    char time_str[16];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);

    /* Broadcast current state - use 0 for interval/delta when no history */
    float interval_sec = 0.0f;
    if (sd->prev_confirmed_ms > 0 && sd->last_confirmed_ms > 0) {
        interval_sec = (sd->last_confirmed_ms - sd->prev_confirmed_ms) / 1000.0f;
    }

    telem_sendf(TELEM_SYNC, "%s,%.1f,%d,%s,%d,%.1f,0,%.1f,%.1f,%.1f",
                time_str, sd->last_confirmed_ms, sd->confirmed_count,
                sync_state_name(sd->state), sd->good_intervals,
                interval_sec,
                sd->pending_tick_duration_ms, sd->pending_marker_duration_ms,
                sd->last_confirmed_ms);
}

/*============================================================================
 * Enhanced API Implementation
 *============================================================================*/

void sync_detector_tick_event(sync_detector_t *sd, float timestamp_ms) {
    if (!sd) return;

    tick_gap_tracker_t *tg = &sd->tick_gap;

    if (tg->last_tick_ms > 0) {
        float gap_ms = timestamp_ms - tg->last_tick_ms;

        if (gap_ms >= TICK_INTERVAL_MIN_MS && gap_ms <= TICK_INTERVAL_MAX_MS) {
            /* Normal tick interval */
            tg->consecutive_tick_count++;

        } else if (gap_ms < TICK_DOUBLE_MAX_GAP_MS) {
            /* DUT1 double-tick - ignore, don't reset counter */
            return;

        } else if (gap_ms >= TICK_HOLE_MIN_GAP_MS && gap_ms <= TICK_HOLE_MAX_GAP_MS) {
            /* Tick hole detected */
            if (tg->consecutive_tick_count >= MIN_TICKS_FOR_HOLE) {
                sync_detector_hole_detected(sd, tg->last_tick_ms + 1000.0f);
            }
            tg->consecutive_tick_count = 1;

        } else if (gap_ms > TICK_HOLE_MAX_GAP_MS) {
            /* Signal loss */
            printf("[SYNC] Tick gap %.0fms - signal loss\n", gap_ms);
            tg->consecutive_tick_count = 1;
        } else {
            /* Short gap - noise */
            return;
        }
    } else {
        tg->consecutive_tick_count = 1;
    }

    tg->last_tick_ms = timestamp_ms;

    /* Apply tick evidence */
    if (sd->state >= SYNC_TENTATIVE) {
        float weight = get_evidence_weight(sd, EVIDENCE_TICK);
        apply_evidence(sd, EVIDENCE_TICK, weight);
    }
}

static float second_wrap(float d) {
    return d - roundf(d / 1000.0f) * 1000.0f;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float median_of(float *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp_float);
    return (n & 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* Offset of an edge from its predicted second under the held phase */
static float edge_offset(const sync_detector_t *sd, const phase_edge_t *e) {
    float lead = e->kind == SYNC_EDGE_TICK ? PHASE_LEAD_TICK_MS : PHASE_LEAD_BTIME_MS;
    return second_wrap(e->t_ms - sd->last_confirmed_ms - lead);
}

static void track_phase(sync_detector_t *sd, float now_ms) {
    float ticks[PHASE_EDGES_MAX], gated[PHASE_EDGES_MAX], gated_ticks[PHASE_EDGES_MAX];
    int nt = 0, ng = 0, ngt = 0;
    for (int i = 0; i < sd->edge_count; i++) {
        const phase_edge_t *e = &sd->edges[(sd->edge_head - sd->edge_count + i + PHASE_EDGES_MAX) % PHASE_EDGES_MAX];
        if (e->t_ms < now_ms - PHASE_JUMP_WINDOW_MS) continue;
        float o = edge_offset(sd, e);
        if (e->kind == SYNC_EDGE_TICK) ticks[nt++] = o;
        if (e->t_ms < now_ms - PHASE_WINDOW_MS || fabsf(o) > PHASE_GATE_MS) continue;
        gated[ng++] = o;
        if (e->kind == SYNC_EDGE_TICK) gated_ticks[ngt++] = o;
    }

    /* A tick cluster off the held phase: a jump of unknown whole seconds */
    if (nt >= PHASE_JUMP_MIN_TICKS) {
        int best = 0, near = 0;
        float best_at = 0.0f;
        for (int i = 0; i < nt; i++) {
            if (fabsf(ticks[i]) <= PHASE_JUMP_GATE_MS) { near++; continue; }
            int n = 0;
            for (int j = 0; j < nt; j++)
                if (fabsf(second_wrap(ticks[j] - ticks[i])) <= PHASE_JUMP_RADIUS_MS) n++;
            if (n > best) { best = n; best_at = ticks[i]; }
        }
        float background = (float)nt * 2.0f * PHASE_JUMP_RADIUS_MS / 1000.0f;
        if (best >= PHASE_JUMP_MIN_TICKS && (float)best >= PHASE_JUMP_BG_RATIO * background &&
            best > 2 * near) {
            char why[64];
            snprintf(why, sizeof why, "%d ticks at %+.0f ms, %d near it", best, best_at, near);
            sd->phase_stats.jumps++;
            /* Samples lost upstream only make the edges early. An early cluster
             * while the phase had support PHASE_SNAP_SUPPORT_MS ago is the loss
             * since then; anything else is ambiguous. */
            if (best_at < 0.0f && sd->phase_support_ms > 0.0f &&
                now_ms - sd->phase_support_ms <= PHASE_SNAP_SUPPORT_MS) {
                note_phase_move(sd, sd->last_confirmed_ms, sd->last_confirmed_ms + best_at, "edge jump");
                sd->last_confirmed_ms += best_at;
                sd->minute_anchor_ms += best_at;
                sd->phase_stats.snaps++;
                sd->phase_support_ms = now_ms;
                return;
            }
            enter_phase_unknown(sd, now_ms, why);
            return;
        }
    }

    /* Slew: the median offset of the in-gate edges, ticks alone when enough */
    float *use = ngt >= PHASE_MIN_EDGES ? gated_ticks : gated;
    int n = ngt >= PHASE_MIN_EDGES ? ngt : ng;
    if (n < PHASE_MIN_EDGES) return;
    sd->phase_support_ms = now_ms;
    float e = median_of(use, n);
    if (fabsf(e) <= (use == gated_ticks ? PHASE_DEADBAND_MS : PHASE_DEADBAND_BTIME_MS)) return;
    float c = PHASE_GAIN * e;
    sd->last_confirmed_ms += c;
    sd->minute_anchor_ms += c;
    sd->phase_stats.slews++;
    sd->phase_stats.slewed_ms += c;
}

void sync_detector_second_edge(sync_detector_t *sd, float timestamp_ms, sync_edge_kind_t kind) {
    if (!sd || sd->state != SYNC_LOCKED || sd->last_confirmed_ms <= 0.0f) return;
    sd->edges[sd->edge_head] = (phase_edge_t){ timestamp_ms, (uint8_t)kind };
    sd->edge_head = (sd->edge_head + 1) % PHASE_EDGES_MAX;
    if (sd->edge_count < PHASE_EDGES_MAX) sd->edge_count++;
    if (timestamp_ms - sd->last_phase_eval_ms < PHASE_EVAL_MS) return;
    sd->last_phase_eval_ms = timestamp_ms;
    track_phase(sd, timestamp_ms);
}

int sync_detector_get_phase_epoch(sync_detector_t *sd) {
    return sd ? sd->phase_epoch : 0;
}

sync_phase_stats_t sync_detector_get_phase_stats(sync_detector_t *sd) {
    sync_phase_stats_t z = {0};
    return sd ? sd->phase_stats : z;
}

void sync_detector_p_marker_event(sync_detector_t *sd, float timestamp_ms,
                                   float duration_ms) {
    if (!sd) return;

    printf("[SYNC] P-marker: %.1fms dur=%.0fms\n", timestamp_ms, duration_ms);

    /* Apply P-marker evidence */
    if (sd->state >= SYNC_TENTATIVE) {
        float weight = get_evidence_weight(sd, EVIDENCE_P_MARKER);
        apply_evidence(sd, EVIDENCE_P_MARKER, weight);
    }

    /* Recovery validation */
    if (sd->state == SYNC_RECOVERING && sd->recovery.has_retained_state) {
        float since_anchor = timestamp_ms - sd->recovery.retained_anchor_ms;
        while (since_anchor < 0) since_anchor += 60000.0f;
        while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

        int position = (int)(since_anchor / 1000.0f + 0.5f);

        /* Check if at valid P position */
        static const int valid_p[] = {0, 9, 19, 29, 39, 49, 59};
        for (int i = 0; i < 7; i++) {
            if (abs(position - valid_p[i]) <= 1) {
                sd->recovery.recovery_p_marker_seen = true;
                printf("[SYNC] Recovery: P-marker at second %d validates anchor\n", position);
                break;
            }
        }
    }
}

void sync_detector_periodic_check(sync_detector_t *sd, float current_ms) {
    if (!sd) return;

    /* A lone pending marker is confirmed PENDING_TIMEOUT_MS after it, not when
     * the next marker event happens to arrive a minute later. */
    check_timeout(sd, current_ms);

    /* Confidence decay */
    float decay_rate = (sd->state == SYNC_RECOVERING) ?
                       sd->confidence_decay_recovering : sd->confidence_decay_normal;
    sd->confidence *= decay_rate;

    /* Signal loss detection (only when LOCKED) - marker-based authority */
    if (sd->state == SYNC_LOCKED) {
        float since_last_marker = current_ms - sd->last_confirmed_ms;

        if (since_last_marker > SYNC_HOLDOVER_MS) {
            sd->signal_weak_count++;
            if (sd->signal_weak_count >= SIGNAL_WEAK_DEBOUNCE) {
                /* Enter recovery */
                sd->recovery.retained_anchor_ms = sd->minute_anchor_ms;
                sd->recovery.signal_lost_ms = current_ms;
                sd->recovery.has_retained_state = true;
                sd->recovery.recovery_start_ms = current_ms;
                sd->recovery.recovery_tick_seen = false;
                sd->recovery.recovery_marker_seen = false;
                sd->recovery.recovery_p_marker_seen = false;
                sd->recovery.phase_unknown = false;

                transition_state(sd, SYNC_RECOVERING);
            }
        } else {
            sd->signal_weak_count = 0;
        }
    }

    /* Recovery state processing */
    if (sd->state == SYNC_RECOVERING) {
        float time_since_loss = current_ms - sd->recovery.signal_lost_ms;
        float time_in_recovery = current_ms - sd->recovery.recovery_start_ms;

        /* A marker on the retained phase re-locks (confirm_marker() sets
         * recovery_marker_seen). Ticks are not required: the chain does not
         * feed them. The held phase, not the decayed confidence, is what is
         * retained, so only the retention window ends recovery. */
        if (sd->recovery.recovery_marker_seen) {
            printf("[SYNC] Recovery validated after %.0fs - returning to LOCKED\n",
                   time_in_recovery / 1000.0f);
            sd->minute_anchor_ms = sd->last_confirmed_ms;
            sd->recovery.phase_unknown = false;
            sd->edge_count = 0;
            if (sd->confidence < sd->confidence_locked_threshold)
                sd->confidence = sd->confidence_locked_threshold;
            transition_state(sd, SYNC_LOCKED);
        } else if (time_since_loss > SYNC_RETENTION_WINDOW_MS) {
            printf("[SYNC] Retention expired (%.0fs) - full reset\n", time_since_loss / 1000.0f);
            sync_detector_full_reset(sd);
            return;
        }
    }

    /* State transition: TENTATIVE → LOCKED */
    if (sd->state == SYNC_TENTATIVE && sd->confidence >= CONFIDENCE_LOCKED_THRESHOLD) {
        transition_state(sd, SYNC_LOCKED);
    }
}

frame_time_t sync_detector_get_frame_time(sync_detector_t *sd) {
    frame_time_t ft = {0};

    if (!sd || sd->state < SYNC_TENTATIVE || sd->minute_anchor_ms <= 0) {
        ft.current_second = -1;
        ft.state = sd ? sd->state : SYNC_ACQUIRING;
        return ft;
    }

    /* Calculate current second from anchor */
    float since_anchor = sd->tick_gap.last_tick_ms - sd->minute_anchor_ms;
    while (since_anchor < 0) since_anchor += 60000.0f;
    while (since_anchor >= 60000.0f) since_anchor -= 60000.0f;

    ft.current_second = (int)(since_anchor / 1000.0f + 0.5f);
    if (ft.current_second < 0) ft.current_second = 0;
    if (ft.current_second > 59) ft.current_second = 59;

    ft.second_start_ms = sd->minute_anchor_ms + (ft.current_second * 1000.0f);
    ft.confidence = sd->confidence;
    ft.evidence_mask = sd->evidence_mask;
    ft.state = sd->state;

    return ft;
}

float sync_detector_get_confidence(sync_detector_t *sd) {
    return sd ? sd->confidence : 0.0f;
}

void sync_detector_set_wwv_clock(sync_detector_t *sd, wwv_clock_t *clk) {
    if (sd) {
        sd->wwv_clock = clk;
    }
}

void sync_detector_set_leap_second_pending(sync_detector_t *sd, bool pending) {
    if (sd) {
        sd->leap_second_pending = pending;
    }
}

void sync_detector_set_state_callback(sync_detector_t *sd,
                                       sync_state_callback_fn callback,
                                       void *user_data) {
    if (sd) {
        sd->state_callback = callback;
        sd->state_callback_user_data = user_data;
    }
}

bool sync_detector_get_pending_tick(sync_detector_t *sd, float *timestamp_ms, float *duration_ms) {
    if (!sd || !sd->tick_pending) {
        return false;
    }
    if (timestamp_ms) *timestamp_ms = sd->pending_tick_ms;
    if (duration_ms) *duration_ms = sd->pending_tick_duration_ms;
    return true;
}

/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

/* Evidence weights */
void sync_detector_set_weight_tick(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.01f || weight > 0.2f) return;
    sd->weight_tick = weight;
}

float sync_detector_get_weight_tick(sync_detector_t *sd) {
    return sd ? sd->weight_tick : WEIGHT_TICK;
}

void sync_detector_set_weight_marker(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.1f || weight > 0.6f) return;
    sd->weight_marker = weight;
}

float sync_detector_get_weight_marker(sync_detector_t *sd) {
    return sd ? sd->weight_marker : WEIGHT_MARKER;
}

void sync_detector_set_weight_p_marker(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.05f || weight > 0.3f) return;
    sd->weight_p_marker = weight;
}

float sync_detector_get_weight_p_marker(sync_detector_t *sd) {
    return sd ? sd->weight_p_marker : WEIGHT_P_MARKER;
}

void sync_detector_set_weight_tick_hole(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.05f || weight > 0.4f) return;
    sd->weight_tick_hole = weight;
}

float sync_detector_get_weight_tick_hole(sync_detector_t *sd) {
    return sd ? sd->weight_tick_hole : WEIGHT_TICK_HOLE;
}

void sync_detector_set_weight_combined(sync_detector_t *sd, float weight) {
    if (!sd || weight < 0.2f || weight > 0.8f) return;
    sd->weight_combined_hole_marker = weight;
}

float sync_detector_get_weight_combined(sync_detector_t *sd) {
    return sd ? sd->weight_combined_hole_marker : WEIGHT_COMBINED_HOLE_MARKER;
}

/* Confidence thresholds */
void sync_detector_set_locked_threshold(sync_detector_t *sd, float threshold) {
    if (!sd || threshold < 0.5f || threshold > 0.9f) return;
    sd->confidence_locked_threshold = threshold;
}

float sync_detector_get_locked_threshold(sync_detector_t *sd) {
    return sd ? sd->confidence_locked_threshold : CONFIDENCE_LOCKED_THRESHOLD;
}

void sync_detector_set_min_retain(sync_detector_t *sd, float threshold) {
    if (!sd || threshold < 0.01f || threshold > 0.2f) return;
    sd->confidence_min_retain = threshold;
}

float sync_detector_get_min_retain(sync_detector_t *sd) {
    return sd ? sd->confidence_min_retain : CONFIDENCE_MIN_RETAIN;
}

void sync_detector_set_tentative_init(sync_detector_t *sd, float confidence) {
    if (!sd || confidence < 0.1f || confidence > 0.5f) return;
    sd->confidence_tentative_init = confidence;
}

float sync_detector_get_tentative_init(sync_detector_t *sd) {
    return sd ? sd->confidence_tentative_init : CONFIDENCE_TENTATIVE_INIT;
}

/* Confidence decay rates */
void sync_detector_set_decay_normal(sync_detector_t *sd, float rate) {
    if (!sd || rate < 0.99f || rate > 0.9999f) return;
    sd->confidence_decay_normal = rate;
}

float sync_detector_get_decay_normal(sync_detector_t *sd) {
    return sd ? sd->confidence_decay_normal : CONFIDENCE_DECAY_NORMAL;
}

void sync_detector_set_decay_recovering(sync_detector_t *sd, float rate) {
    if (!sd || rate < 0.90f || rate > 0.99f) return;
    sd->confidence_decay_recovering = rate;
}

float sync_detector_get_decay_recovering(sync_detector_t *sd) {
    return sd ? sd->confidence_decay_recovering : CONFIDENCE_DECAY_RECOVERING;
}

/* Validation tolerances */
void sync_detector_set_tick_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 50.0f || ms > 200.0f) return;
    sd->tick_phase_tolerance_ms = ms;
}

float sync_detector_get_tick_tolerance(sync_detector_t *sd) {
    return sd ? sd->tick_phase_tolerance_ms : TICK_PHASE_TOLERANCE_MS;
}

void sync_detector_set_marker_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 200.0f || ms > 800.0f) return;
    sd->marker_tolerance_ms = ms;
}

float sync_detector_get_marker_tolerance(sync_detector_t *sd) {
    return sd ? sd->marker_tolerance_ms : MARKER_TOLERANCE_MS;
}

void sync_detector_set_p_marker_tolerance(sync_detector_t *sd, float ms) {
    if (!sd || ms < 100.0f || ms > 400.0f) return;
    sd->p_marker_tolerance_ms = ms;
}

float sync_detector_get_p_marker_tolerance(sync_detector_t *sd) {
    return sd ? sd->p_marker_tolerance_ms : P_MARKER_TOLERANCE_MS;
}
