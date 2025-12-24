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
    float window_start_ms;
    float window_anchor_ms;

    /* Energy accumulation for current window */
    float time_energy_sum;
    float time_duration_sum;
    int time_event_count;
    float time_first_ms;
    float time_last_ms;

    float freq_energy_sum;
    float freq_duration_sum;
    int freq_event_count;
    float freq_first_ms;
    float freq_last_ms;

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
 * Calculate which second (0-59) a timestamp falls into
 * Returns -1 if cannot determine
 */
int bcd_window_get_second_for_timestamp(bcd_correlator_t *corr, float timestamp_ms, float anchor_ms);

/**
 * Calculate window start time for a given second
 */
float bcd_window_get_start(float anchor_ms, int second);

/**
 * Estimate pulse duration from accumulated events
 */
float bcd_window_estimate_pulse_duration(bcd_correlator_t *corr);

/**
 * Open a new integration window
 */
void bcd_window_open(bcd_correlator_t *corr, int second, float anchor_ms);

/**
 * Close current window and emit symbol
 */
void bcd_window_close(bcd_correlator_t *corr);

/**
 * Check if window transition is needed and handle it
 * Called on every event
 */
void bcd_window_check_transition(bcd_correlator_t *corr, float timestamp_ms);

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
