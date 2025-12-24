/**
 * @file tick_chain_manager.c
 * @brief Tick correlation chain statistics tracking
 *
 * Extracted from tick_correlator.c to separate chain statistics management
 * from correlation decision logic.
 *
 * Contains:
 *   - Chain initialization
 *   - Chain statistics updates
 */

#include "tick_correlator_internal.h"
#include <string.h>

/*============================================================================
 * Chain Management Functions
 *============================================================================*/

void tick_chain_start_new(tick_correlator_t *tc, float timestamp_ms) {
    tc->chain_count++;
    tc->current_chain_id = tc->chain_count;
    tc->current_chain_length = 0;
    tc->current_chain_start_ms = timestamp_ms;
    tc->cumulative_drift_ms = 0.0f;

    /* Reset interval tracking for epoch calculation */
    tc->recent_interval_idx = 0;
    tc->recent_interval_count = 0;
    memset(tc->recent_intervals, 0, sizeof(tc->recent_intervals));

    /* Initialize chain stats */
    if (tc->chain_count <= tc->chain_capacity) {
        chain_stats_t *cs = &tc->chains[tc->chain_count - 1];
        cs->chain_id = tc->current_chain_id;
        cs->tick_count = 0;
        cs->inferred_count = 0;
        cs->start_ms = timestamp_ms;
        cs->end_ms = timestamp_ms;
        cs->total_drift_ms = 0.0f;
        cs->avg_interval_ms = 0.0f;
        cs->min_interval_ms = 99999.0f;
        cs->max_interval_ms = 0.0f;
    }
}

void tick_chain_update_stats(tick_correlator_t *tc, float interval_ms, float timestamp_ms) {
    if (tc->current_chain_id <= 0 || tc->current_chain_id > tc->chain_capacity) return;

    chain_stats_t *cs = &tc->chains[tc->current_chain_id - 1];
    cs->tick_count = tc->current_chain_length;
    cs->end_ms = timestamp_ms;
    cs->total_drift_ms = tc->cumulative_drift_ms;

    /* Update interval stats */
    if (interval_ms > 0) {
        if (interval_ms < cs->min_interval_ms) cs->min_interval_ms = interval_ms;
        if (interval_ms > cs->max_interval_ms) cs->max_interval_ms = interval_ms;

        /* Running average */
        float n = (float)cs->tick_count;
        cs->avg_interval_ms = ((n - 1.0f) * cs->avg_interval_ms + interval_ms) / n;
    }
}
