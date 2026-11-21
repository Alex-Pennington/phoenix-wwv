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

/* WWV's position markers. Second 0 (the WWVB frame reference) is not one: WWV
 * sends no pulse there, and a long event at second 0 labelled P made the time
 * decoder refuse the frame. On the live replays every such frame was on the
 * right phase (run90d 11:36 and 12:03, run90c, run90, run90f, run3h). */
const int VALID_P_POSITIONS[] = {9, 19, 29, 39, 49, 59, -1};

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
    /* Each event carries its own measured pulse width. With several events in
     * one window, the first is the one nearest the second's start, where the
     * BCD pulse begins; later ones are mid-second detections. The distance
     * between two event starts is not a pulse width. */
    float time_span = 0.0f;
    float freq_span = 0.0f;

    if (corr->time_event_count >= 1) {
        time_span = corr->time_first_dur_ms;
    }

    if (corr->freq_event_count >= 1) {
        freq_span = corr->freq_first_dur_ms;
    }

    /* BTIME measures the pulse width. BFREQ thresholds a 1 s sliding sum of the
     * subcarrier and fires mostly on the P-marker clusters; its durations do not
     * cluster at 200/500/800 ms, so it is used only when BTIME has no event. */
    if (time_span > 0) {
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
