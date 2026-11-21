/**
 * @file sync_detector.h
 * @brief WWV unified sync detector - correlates all timing evidence
 *
 * This detector receives events from multiple sources and fuses them into
 * a single authoritative timing reference:
 *   - tick_detector: 1000Hz pulses every second
 *   - marker_detector: 800ms minute markers
 *   - P-marker events: 800ms BCD position markers at known positions
 *   - Tick holes: absence of ticks at seconds 29 and 59
 *
 * State progression:
 *   ACQUIRING -> TENTATIVE -> LOCKED <-> RECOVERING
 *
 * Once locked, provides authoritative frame timing including current second
 * within the minute, enabling downstream BCD decode without timestamp math.
 *
 * Evidence fusion with confidence tracking allows graceful handling of
 * signal loss and recovery without full re-acquisition.
 */

#ifndef SYNC_DETECTOR_H
#define SYNC_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include "wwv_time.h"

/* Forward declaration for optional wwv_clock integration */
struct wwv_clock;
typedef struct wwv_clock wwv_clock_t;

/*============================================================================
 * Evidence Types
 *============================================================================*/

#define EVIDENCE_TICK        (1 << 0)   /* 1000Hz tick detected */
#define EVIDENCE_MARKER      (1 << 1)   /* Minute marker detected */
#define EVIDENCE_P_MARKER    (1 << 2)   /* BCD P-marker detected */
#define EVIDENCE_TICK_HOLE   (1 << 3)   /* Tick absence at :29/:59 */

/*============================================================================
 * Types
 *============================================================================*/

/* Sync confidence states */
typedef enum {
    SYNC_ACQUIRING,    /* No prior state, searching */
    SYNC_TENTATIVE,    /* Some evidence, building confidence */
    SYNC_LOCKED,       /* High confidence, tracking */
    SYNC_RECOVERING    /* Signal lost, using retained state */
} sync_state_t;

/**
 * Unified frame time - authoritative timing output
 * Consumers use this instead of raw anchor timestamps
 */
typedef struct {
    int current_second;         /* 0-59, authoritative position in minute */
    wwv_time_ms_t second_start_ms; /* When this second began (stream timestamp) */
    float confidence;           /* 0.0 - 1.0, sync quality indicator */
    uint32_t evidence_mask;     /* Which signals contributed to this position */
    sync_state_t state;         /* Current sync state */
} frame_time_t;

/* Opaque handle */
typedef struct sync_detector sync_detector_t;

/*============================================================================
 * API
 *============================================================================*/

/**
 * Create sync detector
 * @param csv_path Path for CSV log file (NULL to disable logging)
 * @return Detector handle or NULL on failure
 */
sync_detector_t *sync_detector_create(const char *csv_path);

/**
 * Destroy sync detector and free resources
 */
void sync_detector_destroy(sync_detector_t *sd);

/**
 * Report tick event for gap tracking and evidence fusion
 * @param sd Detector handle
 * @param timestamp_ms When tick occurred
 */
void sync_detector_tick_event(sync_detector_t *sd, wwv_time_ms_t timestamp_ms);

/* Edges that mark whole seconds, for sync_detector_second_edge() */
typedef enum {
    SYNC_EDGE_TICK = 0,     /* tick_event_t.timestamp_ms */
    SYNC_EDGE_BTIME = 1     /* BCD time-detector pulse leading edge (bcd_time_event_t.timestamp_ms) */
} sync_edge_kind_t;

typedef struct {
    int slews;              /* phase corrections from edges */
    float slewed_ms;        /* their sum */
    int jumps;              /* tick clusters off the held phase */
    int snaps;              /* of those, early, BCD starts agreeing, soon after support: phase moved */
    int jumps_ignored;      /* of those, BCD starts holding the phase: cluster taken as false */
    int candidate_seconds;  /* candidates whole seconds off the held phase (-> RECOVERING) */
    int breaks;             /* phase moves >= 100 ms by markers */
} sync_phase_stats_t;

/**
 * Report an edge at a whole second, to track the sub-second phase while LOCKED.
 * Only its time is used (never a pulse's width). It never starts a lock, never
 * chooses the minute and never moves the phase by whole seconds; a jump it
 * cannot follow drops sync to RECOVERING until a marker re-locks. Ignored when
 * not LOCKED. See PHASE_* in sync_detector.c.
 */
void sync_detector_second_edge(sync_detector_t *sd, wwv_time_ms_t timestamp_ms, sync_edge_kind_t kind);

/**
 * Phase epoch: incremented whenever the held phase moves by 100 ms or more (a
 * marker re-anchor) or is lost (a jump, a candidate whole seconds off). A BCD
 * window or time-decoder frame that spans a change of epoch is discarded.
 */
int sync_detector_get_phase_epoch(sync_detector_t *sd);

/**
 * Phase resets: incremented when a marker moves the held phase by 900 ms or
 * more, or a candidate lands whole seconds off it: the phase was a whole second
 * off for an unknown time before that. The time decoder drops its chain when
 * the count changes.
 */
int sync_detector_get_phase_resets(sync_detector_t *sd);

/** Counts of what the phase tracker did */
sync_phase_stats_t sync_detector_get_phase_stats(sync_detector_t *sd);

/**
 * Report minute marker from tick detector (duration-based detection)
 * @param sd Detector handle
 * @param timestamp_ms Leading edge of the marker (tick_marker_event_t.start_timestamp_ms).
 *                     It becomes the minute anchor, so it must be the pulse start.
 * @param duration_ms Duration of the pulse (should be ~800ms)
 * @param corr_ratio Correlation ratio from tick detector
 */
void sync_detector_tick_marker(sync_detector_t *sd, wwv_time_ms_t timestamp_ms,
                                float duration_ms, float corr_ratio);

/**
 * Report minute marker from marker detector (energy accumulation-based)
 * @param sd Detector handle
 * @param timestamp_ms Leading edge of the marker (marker_event_t.start_timestamp_ms).
 *                     It becomes the minute anchor. marker_event_t.timestamp_ms is the
 *                     end of the 1 s window run, 1.3-1.8 s after the pulse starts.
 * @param accum_energy Accumulated energy in detection window
 * @param duration_ms Duration of accumulated energy above threshold
 */
void sync_detector_marker_event(sync_detector_t *sd, wwv_time_ms_t timestamp_ms,
                                 float accum_energy, float duration_ms);

/**
 * Report a marker candidate: a run a detector rejected as a marker that may be
 * one (tick_marker_candidate_t, or a marker-detector short run). It confirms the
 * minute phase already held when either leading-edge reading lands within the
 * candidate tolerance of a later whole minute from the anchor; the confirmed time
 * is the held phase. Ignored in ACQUIRING, while a marker event is pending, and in
 * a minute already confirmed. While LOCKED, a candidate whose end reading
 * (lead_b_ms) is 1-5 whole seconds off the held phase sends sync to RECOVERING
 * with the phase marked unknown; the start reading never does.
 * @param lead_a_ms, lead_b_ms Two readings of the leading edge: from the run's
 *        start and from its end (equal if one)
 * @param source Label for the console and the marker CSV ("CANDT" tick path, "CANDM" marker detector)
 */
void sync_detector_marker_candidate(sync_detector_t *sd, wwv_time_ms_t lead_a_ms, wwv_time_ms_t lead_b_ms,
                                    const char *source);

/**
 * Get unified frame time - authoritative timing output
 * @param sd Detector handle
 * @return frame_time_t with current second, confidence, evidence
 */
frame_time_t sync_detector_get_frame_time(sync_detector_t *sd);

/**
 * Get confidence value (0.0 - 1.0)
 * @param sd Detector handle
 * @return Current confidence
 */
float sync_detector_get_confidence(sync_detector_t *sd);

/**
 * Get state as string for display
 */
const char *sync_state_name(sync_state_t state);

/**
 * Get timestamp of last confirmed marker (backward compatibility)se (~800ms)
 */
void sync_detector_p_marker_event(sync_detector_t *sd, wwv_time_ms_t timestamp_ms,
                                   float duration_ms);

/**
 * Periodic maintenance - check for signal loss, decay confidence
 * Call every ~100ms from sample processing loop
 * @param sd Detector handle
 * @param current_ms Current stream timestamp
 */
void sync_detector_periodic_check(sync_detector_t *sd, wwv_time_ms_t current_ms);

/**
 * Get current sync state
 */
sync_state_t sync_detector_get_state(sync_detector_t *sd);

/**
 * Get state as string for display
 */
const char *sync_state_name(sync_state_t state);

/**
 * Get timestamp of last confirmed marker
 */
wwv_time_ms_t sync_detector_get_last_marker_ms(sync_detector_t *sd);


/**
 * Get count of confirmed markers
 */
int sync_detector_get_confirmed_count(sync_detector_t *sd);

/**
 * Get flash frames remaining (for UI feedback)
 */
int sync_detector_get_flash_frames(sync_detector_t *sd);

/**
 * Decrement flash counter (call each frame)
 */
void sync_detector_decrement_flash(sync_detector_t *sd);

/**
 * Set optional wwv_clock for schedule-aware evidence weighting
 * @param sd Detector handle
 * @param clk WWV clock instance (NULL to disable)
 */
void sync_detector_set_wwv_clock(sync_detector_t *sd, wwv_clock_t *clk);

/**
 * Set leap second pending flag (affects timing tolerances)
 * @param sd Detector handle
 * @param pending True if leap second is pending
 */
void sync_detector_set_leap_second_pending(sync_detector_t *sd, bool pending);

/**
 * Set callback for state transitions
 * @param sd Detector handle
 * @param callback Function to call on state change
 * @param user_data User data passed to callback
 */
typedef void (*sync_state_callback_fn)(sync_state_t old_state, sync_state_t new_state,
                                        float confidence, void *user_data);
void sync_detector_set_state_callback(sync_detector_t *sd,
                                       sync_state_callback_fn callback,
                                       void *user_data);

/**
 * Broadcast current sync state via UDP telemetry
 * Call on startup and whenever state needs to be reported
 */
void sync_detector_broadcast_state(sync_detector_t *sd);

/**
 * Get count of good (~60s) intervals for lock confidence
 */
int sync_detector_get_good_intervals(sync_detector_t *sd);

/**
 * Get pending tick marker info (for precise epoch calculation)
 * @param sd Detector handle
 * @param timestamp_ms Output: tick marker trailing edge timestamp (can be NULL)
 * @param duration_ms Output: tick marker duration (can be NULL)
 * @return true if tick marker is pending, false otherwise
 */
bool sync_detector_get_pending_tick(sync_detector_t *sd, wwv_time_ms_t *timestamp_ms, float *duration_ms);


/*============================================================================
 * Runtime Parameter Tuning
 *============================================================================*/

/* Evidence weights */
void sync_detector_set_weight_tick(sync_detector_t *sd, float weight);
float sync_detector_get_weight_tick(sync_detector_t *sd);
void sync_detector_set_weight_marker(sync_detector_t *sd, float weight);
float sync_detector_get_weight_marker(sync_detector_t *sd);
void sync_detector_set_weight_p_marker(sync_detector_t *sd, float weight);
float sync_detector_get_weight_p_marker(sync_detector_t *sd);
void sync_detector_set_weight_tick_hole(sync_detector_t *sd, float weight);
float sync_detector_get_weight_tick_hole(sync_detector_t *sd);
void sync_detector_set_weight_combined(sync_detector_t *sd, float weight);
float sync_detector_get_weight_combined(sync_detector_t *sd);

/* Confidence thresholds */
void sync_detector_set_locked_threshold(sync_detector_t *sd, float threshold);
float sync_detector_get_locked_threshold(sync_detector_t *sd);
void sync_detector_set_min_retain(sync_detector_t *sd, float threshold);
float sync_detector_get_min_retain(sync_detector_t *sd);
void sync_detector_set_tentative_init(sync_detector_t *sd, float confidence);
float sync_detector_get_tentative_init(sync_detector_t *sd);

/* Confidence decay rates */
void sync_detector_set_decay_normal(sync_detector_t *sd, float rate);
float sync_detector_get_decay_normal(sync_detector_t *sd);
void sync_detector_set_decay_recovering(sync_detector_t *sd, float rate);
float sync_detector_get_decay_recovering(sync_detector_t *sd);

/* Validation tolerances */
void sync_detector_set_tick_tolerance(sync_detector_t *sd, float ms);
float sync_detector_get_tick_tolerance(sync_detector_t *sd);
void sync_detector_set_marker_tolerance(sync_detector_t *sd, float ms);
float sync_detector_get_marker_tolerance(sync_detector_t *sd);
void sync_detector_set_p_marker_tolerance(sync_detector_t *sd, float ms);
float sync_detector_get_p_marker_tolerance(sync_detector_t *sd);

#endif /* SYNC_DETECTOR_H */
