/**
 * @file bcd_window_manager.c
 * @brief BCD correlator window management logic
 *
 * Separated from bcd_correlator.c to isolate window timing and
 * energy accumulation from symbol classification.
 *
 * Contains:
 *   - Window timing calculations
 *   - Window open/close logic
 *   - Energy accumulation
 *   - Symbol emission
 */

#include "bcd_correlator_internal.h"
#include "telemetry.h"
#include "version.h"
#include <string.h>
#include <stdio.h>

/*============================================================================
 * Window Timing Functions
 *============================================================================*/

float bcd_window_get_minute_anchor(bcd_correlator_t *corr) {
    if (!corr->sync_source) return -1.0f;
    if (sync_detector_get_state(corr->sync_source) != SYNC_LOCKED) return -1.0f;
    return sync_detector_get_last_marker_ms(corr->sync_source);
}

int bcd_window_get_second_for_timestamp(bcd_correlator_t *corr, float timestamp_ms, float anchor_ms) {
    if (anchor_ms < 0) return -1;

    float offset_ms = timestamp_ms - anchor_ms;

    /* Handle wrap-around for new minute */
    while (offset_ms < 0) offset_ms += 60000.0f;
    while (offset_ms >= 60000.0f) offset_ms -= 60000.0f;

    int second = (int)(offset_ms / WINDOW_DURATION_MS);
    if (second < 0) second = 0;
    if (second > 59) second = 59;

    return second;
}

float bcd_window_get_start(float anchor_ms, int second) {
    return anchor_ms + (second * WINDOW_DURATION_MS);
}

/*============================================================================
 * Window Management Functions
 *============================================================================*/

void bcd_window_open(bcd_correlator_t *corr, int second, float anchor_ms) {
    corr->window_open = true;
    corr->current_second = second;
    corr->window_start_ms = bcd_window_get_start(anchor_ms, second);
    corr->window_anchor_ms = anchor_ms;

    /* Reset accumulators */
    corr->time_energy_sum = 0.0f;
    corr->time_duration_sum = 0.0f;
    corr->time_event_count = 0;
    corr->time_first_ms = 0.0f;
    corr->time_last_ms = 0.0f;

    corr->freq_energy_sum = 0.0f;
    corr->freq_duration_sum = 0.0f;
    corr->freq_event_count = 0;
    corr->freq_first_ms = 0.0f;
    corr->freq_last_ms = 0.0f;
}

void bcd_window_close(bcd_correlator_t *corr) {
    if (!corr->window_open) return;

    int total_events = corr->time_event_count + corr->freq_event_count;
    float total_energy = corr->time_energy_sum + corr->freq_energy_sum;

    /* Determine confidence and source */
    float confidence = 0.0f;
    const char *source = "NONE";

    if (corr->time_event_count > 0 && corr->freq_event_count > 0) {
        source = "BOTH";
        confidence = 1.0f;
    } else if (corr->time_event_count > 0) {
        source = "TIME";
        confidence = 0.6f;
    } else if (corr->freq_event_count > 0) {
        source = "FREQ";
        confidence = 0.6f;
    }

    /* Estimate pulse duration */
    float duration_ms = bcd_window_estimate_pulse_duration(corr);

    /* Classify symbol (Phase 8: with position gating) */
    bcd_corr_symbol_t symbol = BCD_CORR_SYM_NONE;

    if (total_events >= MIN_EVENTS_FOR_SYMBOL && total_energy > ENERGY_THRESHOLD_LOW) {
        symbol = bcd_symbol_classify_duration(duration_ms, corr->current_second);
    } else if (total_events > 0) {
        /* Some events but not enough confidence - still classify but lower confidence */
        symbol = bcd_symbol_classify_duration(duration_ms, corr->current_second);
        confidence *= 0.5f;
    }
    /* If no events at all, symbol stays NONE (no 100Hz detected this second) */

    /* Calculate timestamp for this symbol (center of window) */
    float symbol_timestamp_ms = corr->window_start_ms + (WINDOW_DURATION_MS / 2.0f);

    /* Track intervals */
    float interval_ms = 0.0f;
    if (corr->last_symbol_ms > 0) {
        interval_ms = symbol_timestamp_ms - corr->last_symbol_ms;
        if (interval_ms >= 900.0f && interval_ms <= 1100.0f) {
            corr->good_intervals++;
        }
    }

    /* Update state machine */
    if (corr->good_intervals >= 3) {
        corr->state = BCD_CORR_TRACKING;
    } else if (corr->symbol_count >= 1) {
        corr->state = BCD_CORR_TENTATIVE;
    }

    /* Update tracking */
    corr->last_symbol_ms = symbol_timestamp_ms;
    corr->symbol_count++;

    /* CSV logging */
    if (corr->csv_file) {
        char time_str[16];
        bcd_corr_get_wall_time_str(corr->start_time, symbol_timestamp_ms, time_str, sizeof(time_str));

        fprintf(corr->csv_file, "%s,%.1f,%d,%c,%.0f,%.1f,%d,%d,%s,%.2f\n",
                time_str, symbol_timestamp_ms, corr->symbol_count,
                bcd_corr_symbol_char(symbol), duration_ms, interval_ms,
                corr->time_event_count, corr->freq_event_count, source, confidence);
        fflush(corr->csv_file);
    }

    /* UDP telemetry */
    telem_sendf(TELEM_BCDS, "SYMBOL,%d,%.1f,%c,%.0f,%d,%d,%s,%.2f",
                corr->symbol_count, symbol_timestamp_ms,
                bcd_corr_symbol_char(symbol), duration_ms,
                corr->time_event_count, corr->freq_event_count, source, confidence);

    /* Callback */
    if (corr->callback) {
        bcd_symbol_event_t event = {
            .symbol = symbol,
            .timestamp_ms = symbol_timestamp_ms,
            .duration_ms = duration_ms,
            .confidence = confidence,
            .source = source
        };
        corr->callback(&event, corr->callback_user_data);
    }

    /* Close window */
    corr->window_open = false;
}

void bcd_window_check_transition(bcd_correlator_t *corr, float timestamp_ms) {
    if (!corr) return;

    /* Get current sync state */
    float anchor_ms = bcd_window_get_minute_anchor(corr);

    /* If sync not locked, close any open window */
    if (anchor_ms < 0) {
        if (corr->window_open) {
            bcd_window_close(corr);
        }
        return;
    }

    /* Determine which second this event belongs to */
    int event_second = bcd_window_get_second_for_timestamp(corr, timestamp_ms, anchor_ms);
    if (event_second < 0) return;  /* Cannot determine - skip */

    /* If window is not open, open it */
    if (!corr->window_open) {
        bcd_window_open(corr, event_second, anchor_ms);
        return;
    }

    /* Check if anchor changed (new minute started) */
    if (anchor_ms != corr->window_anchor_ms) {
        /* Minute wrapped - close old window, open new one */
        bcd_window_close(corr);
        bcd_window_open(corr, event_second, anchor_ms);
        return;
    }

    /* Check if we moved to a different second */
    if (event_second != corr->current_second) {
        /* Close current window, open new one */
        bcd_window_close(corr);
        bcd_window_open(corr, event_second, anchor_ms);
        return;
    }

    /* Still in same window - no action needed */
}
