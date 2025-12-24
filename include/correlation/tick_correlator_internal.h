/**
 * @file tick_correlator_internal.h
 * @brief Internal structures and function declarations for tick correlator
 *
 * Exposes internals to allow separation of chain management and prediction
 * logic from public API.
 */

#ifndef TICK_CORRELATOR_INTERNAL_H
#define TICK_CORRELATOR_INTERNAL_H

#include "tick_correlator.h"
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Internal Configuration
 *============================================================================*/

#define MAX_CHAINS          1000
#define MAX_TICKS_STORED    10000

/*============================================================================
 * Tick Correlator Internal Structure
 *============================================================================*/

struct tick_correlator {
    /* Tick storage */
    tick_record_t *ticks;
    int tick_count;
    int tick_capacity;

    /* Chain tracking */
    chain_stats_t *chains;
    int chain_count;
    int chain_capacity;

    /* Current chain state */
    int current_chain_id;
    int current_chain_length;
    float current_chain_start_ms;
    float last_tick_ms;
    float cumulative_drift_ms;

    /* Overall stats */
    int total_correlated;
    int total_uncorrelated;
    float longest_chain_ticks;

    /* Logging */
    FILE *csv_file;
    time_t start_time;

    /* Epoch callback */
    epoch_callback_fn epoch_callback;
    void *epoch_callback_user_data;

    /* Interval tracking for std_dev calculation */
    float recent_intervals[5];
    int recent_interval_idx;
    int recent_interval_count;

    /* Prediction-based tracking state */
    struct {
        bool active;
        int retained_chain_id;
        float predicted_next_ms;
        float discipline_window_ms;
        float last_std_dev_ms;
        int consecutive_misses;
    } tracking;

    /* Tunable parameters */
    float epoch_confidence_threshold;
    int max_consecutive_misses;
};

/*============================================================================
 * Chain Management Functions (tick_chain_manager.c)
 *============================================================================*/

/**
 * Start a new correlation chain
 */
void tick_chain_start_new(tick_correlator_t *tc, float timestamp_ms);

/**
 * Update chain statistics with new interval
 */
void tick_chain_update_stats(tick_correlator_t *tc, float interval_ms, float timestamp_ms);

/**
 * Determine if tick correlates with current chain
 * Returns: 0=no correlation, 1=normal correlation, 2=single-skip correlation
 */
int tick_chain_correlate(tick_correlator_t *tc, float actual_interval, bool prediction_match);

/*============================================================================
 * Prediction/Epoch Functions (tick_predictor.c)
 *============================================================================*/

/**
 * Check if tick matches prediction from established discipline
 * Returns true if within discipline window
 */
bool tick_predict_match(tick_correlator_t *tc, float timestamp_ms, float actual_interval);

/**
 * Calculate epoch when sufficient correlation established
 * Calls epoch callback if confidence threshold met
 */
void tick_predict_calculate_epoch(tick_correlator_t *tc, float timestamp_ms);

/**
 * Track recent intervals for epoch calculation
 */
void tick_predict_track_interval(tick_correlator_t *tc, float interval_ms);

#ifdef __cplusplus
}
#endif

#endif /* TICK_CORRELATOR_INTERNAL_H */
