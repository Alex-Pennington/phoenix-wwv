/**
 * @file bcd_symbol_classifier.c
 * @brief BCD symbol classification logic
 *
 * Separated from bcd_correlator.c to isolate symbol classification
 * concerns from window management.
 *
 * Contains:
 *   - Pulse duration estimation
 *   - P-marker position validation
 *   - Symbol classification (0/1/P)
 */

#include "bcd_correlator_internal.h"

/*============================================================================
 * Phase 8: Valid P-marker Positions
 *============================================================================*/

const int VALID_P_POSITIONS[] = {0, 9, 19, 29, 39, 49, 59, -1};

/*============================================================================
 * Symbol Classification Functions
 *============================================================================*/

bool bcd_symbol_is_valid_p_position(int second) {
    for (int i = 0; VALID_P_POSITIONS[i] >= 0; i++) {
        if (VALID_P_POSITIONS[i] == second) return true;
    }
    return false;
}

bcd_corr_symbol_t bcd_symbol_classify_duration(float duration_ms, int second) {
    if (duration_ms < 100.0f) {
        return BCD_CORR_SYM_NONE;  /* Too short - no signal */
    } else if (duration_ms <= BCD_SYMBOL_ZERO_MAX_MS) {
        return BCD_CORR_SYM_ZERO;  /* 100-350ms = binary 0 */
    } else if (duration_ms <= BCD_SYMBOL_ONE_MAX_MS) {
        return BCD_CORR_SYM_ONE;   /* 350-650ms = binary 1 */
    } else if (duration_ms <= BCD_SYMBOL_MARKER_MAX_MS) {
        /* Phase 8: Position gating - only call it P-marker if at valid position */
        if (bcd_symbol_is_valid_p_position(second)) {
            return BCD_CORR_SYM_MARKER; /* 650-900ms = position marker */
        } else {
            return BCD_CORR_SYM_ONE;  /* Downgrade to ONE if wrong position */
        }
    }
    /* >900ms at valid position = marker, otherwise ONE */
    return bcd_symbol_is_valid_p_position(second) ? BCD_CORR_SYM_MARKER : BCD_CORR_SYM_ONE;
}

float bcd_window_estimate_pulse_duration(bcd_correlator_t *corr) {
    float time_span = 0.0f;
    float freq_span = 0.0f;

    if (corr->time_event_count >= 2) {
        time_span = corr->time_last_ms - corr->time_first_ms;
    } else if (corr->time_event_count == 1) {
        /* Single event - use reported duration */
        time_span = corr->time_duration_sum;
    }

    if (corr->freq_event_count >= 2) {
        freq_span = corr->freq_last_ms - corr->freq_first_ms;
    } else if (corr->freq_event_count == 1) {
        freq_span = corr->freq_duration_sum;
    }

    /* If we have both, average them; otherwise use whichever we have */
    if (time_span > 0 && freq_span > 0) {
        return (time_span + freq_span) / 2.0f;
    } else if (time_span > 0) {
        return time_span;
    } else if (freq_span > 0) {
        return freq_span;
    }

    /* Fallback: estimate from average durations reported */
    float avg_dur = 0.0f;
    int count = 0;
    if (corr->time_event_count > 0) {
        avg_dur += corr->time_duration_sum / corr->time_event_count;
        count++;
    }
    if (corr->freq_event_count > 0) {
        avg_dur += corr->freq_duration_sum / corr->freq_event_count;
        count++;
    }

    return (count > 0) ? avg_dur / count : 0.0f;
}
