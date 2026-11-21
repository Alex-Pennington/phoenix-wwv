/**
 * @file bcd_correlator_internal.h
 * @brief Internal structures and function declarations for BCD correlator
 *
 * Exposes internals to allow separation of window management and
 * symbol classification from public API.
 *
 * Pattern: Follows detector internal headers
 */

#ifndef BCD_CORRELATOR_INTERNAL_H
#define BCD_CORRELATOR_INTERNAL_H

#include "bcd_correlator.h"
#include "sync_detector.h"
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Internal Configuration
 *============================================================================*/

/* Window timing */
#define WINDOW_DURATION_MS      1000.0f
#define WINDOW_TOLERANCE_MS     50.0f
/* Window k opens WINDOW_LEAD_MS before second k of the anchor. A BCD pulse
 * starts at the second; the longest (P, 800 ms) ends by +800 ms, so no real
 * pulse starts in the last 200 ms of a second. The edge sits in the middle of
 * that gap: a pulse detected up to 100 ms before its anchor second (anchor
 * late, start jitter) still lands in its own window. */
#define WINDOW_LEAD_MS          100.0f
/* BTIME and BFREQ report a pulse at its end and cap it at 1100 / 1000 ms, so
 * every event for a window arrives by the window's end + 1100 ms (plus BTIME's
 * 3-frame end debounce: an event starting in the last 15 ms of a window and
 * running the full 1100 ms is late, and dropped from that mid-second window;
 * its trailing edge is still noted). The periodic check closes a window this
 * long after its end. */
#define WINDOW_CLOSE_LATENCY_MS 1100.0f

/* Trailing-edge symbols. When the BTIME detector starts an event on the
 * low-level subcarrier (after a cooldown, in a fade), that event runs on through
 * the next second's pulse and ends at its trailing edge; the next event starts
 * mid-second, and the chain repeats. The window then holds no event at its
 * second, but an event that started in an earlier window ends inside it, at the
 * end of this second's pulse. A window whose second holds such an end (at least
 * EDGE_MIN_END_MS after the second) takes its width from it: the end less
 * EDGE_LEAD_MS, where the detector's leading edge lands (+39..+42 ms median from
 * the anchor on the live runs, +44..+48 ms from UTC on the captures). */
#define EDGE_MIN_END_MS         100.0f
#define EDGE_LEAD_MS            40.0f

/* Phase 8: Valid P-marker positions (WWV BCD time code format) */
extern const int VALID_P_POSITIONS[];

/* Energy integration thresholds */
#define MIN_EVENTS_FOR_SYMBOL   2
#define ENERGY_THRESHOLD_LOW    0.001f

/*============================================================================
 * BCD Correlator Internal Structure
 *============================================================================*/

struct bcd_correlator {
    /* Sync source - provides timing reference */
    sync_detector_t *sync_source;

    /* Current window state */
    bool window_open;
    int current_second;             /* Which second (0-59) */
    int window_index;               /* Whole seconds from the anchor, unwrapped */
    float window_start_ms;
    float window_anchor_ms;
    bool window_partial;            /* opened after its start (lock began in it) */
    int late_events;                /* events for a window already emitted */
    int silent_windows;             /* windows emitted as NONE: locked, no event */
    int window_epoch;               /* sync phase epoch the window was opened in */
    int window_resets;              /* sync phase resets when it was opened */
    int broken_windows;             /* windows discarded: the phase broke inside them */

    /* Energy accumulation for current window */
    float time_energy_sum;
    float time_duration_sum;
    int time_event_count;
    float time_first_ms;
    float time_first_dur_ms;        /* reported width of the window's first event */
    float time_last_ms;

    float freq_energy_sum;
    float freq_duration_sum;
    int freq_event_count;
    float freq_first_ms;
    float freq_first_dur_ms;
    float freq_last_ms;

    /* A BTIME event that started in an earlier window and ends in this one
     * (see EDGE_MIN_END_MS), and one that ends in a window not yet open */
    bool edge_valid;
    float edge_start_ms, edge_end_ms;
    bool edge_pending;
    float edge_pending_start_ms, edge_pending_end_ms;

    /* Symbol tracking */
    float last_symbol_ms;
    int symbol_count;
    int good_intervals;

    /* State machine */
    bcd_corr_state_t state;

    /* Callback */
    bcd_corr_symbol_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Common Helper Functions
 *============================================================================*/

/**
 * Get wall clock time string for CSV output
 */
static inline void bcd_corr_get_wall_time_str(time_t start_time, float timestamp_ms,
                                               char *buf, size_t buflen) {
    time_t event_time = start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/*============================================================================
 * Window Management Functions (bcd_window_manager.c)
 *============================================================================*/

/**
 * Get the current minute anchor from sync detector
 * Returns -1 if sync is not locked
 */
float bcd_window_get_minute_anchor(bcd_correlator_t *corr);

/**
 * Window index of a timestamp: whole seconds from the anchor (after the window
 * lead), not wrapped at 60, so a stale anchor still gives the right window start
 */
int bcd_window_get_index(float timestamp_ms, float anchor_ms);

/**
 * Second in the minute (0-59) of a window index
 */
int bcd_window_second_of_index(int index);

/**
 * Calculate which second (0-59) a timestamp falls into
 * Returns -1 if cannot determine
 */
int bcd_window_get_second_for_timestamp(bcd_correlator_t *corr, float timestamp_ms, float anchor_ms);

/**
 * Calculate window start time for a window index
 */
float bcd_window_get_start(float anchor_ms, int index);

/**
 * Estimate pulse duration from accumulated events
 */
float bcd_window_estimate_pulse_duration(bcd_correlator_t *corr);

/**
 * Open a new integration window
 */
void bcd_window_open(bcd_correlator_t *corr, int index, float anchor_ms);

/**
 * Close current window and emit symbol
 */
void bcd_window_close(bcd_correlator_t *corr);

/**
 * Check if window transition is needed and handle it
 * Called on every event. Returns false when the event is not to be accumulated
 * (sync not locked, or its window was already emitted).
 */
bool bcd_window_check_transition(bcd_correlator_t *corr, float timestamp_ms);

/**
 * Note a BTIME event's span: when it ends in a later window than it started in,
 * its end is the trailing edge of that window's pulse (see EDGE_MIN_END_MS)
 */
void bcd_window_note_edge(bcd_correlator_t *corr, float start_ms, float end_ms);

/**
 * Close windows whose events have all arrived, by stream time
 */
void bcd_window_periodic(bcd_correlator_t *corr, float now_ms);

/*============================================================================
 * Symbol Classification Functions (bcd_symbol_classifier.c)
 *============================================================================*/

/**
 * Check if second position is valid for P-marker
 */
bool bcd_symbol_is_valid_p_position(int second);

/**
 * Classify pulse duration into symbol type
 * Applies position gating for P-markers
 */
bcd_corr_symbol_t bcd_symbol_classify_duration(float duration_ms, int second);

#ifdef __cplusplus
}
#endif

#endif /* BCD_CORRELATOR_INTERNAL_H */
