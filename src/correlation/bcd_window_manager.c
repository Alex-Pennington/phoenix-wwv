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
#include <math.h>
#include <stdio.h>

/*============================================================================
 * Window Timing Functions
 *============================================================================*/

float bcd_window_get_minute_anchor(bcd_correlator_t *corr) {
    if (!corr->sync_source) return -1.0f;
    if (sync_detector_get_state(corr->sync_source) != SYNC_LOCKED) return -1.0f;
    return sync_detector_get_last_marker_ms(corr->sync_source);
}

int bcd_window_get_index(float timestamp_ms, float anchor_ms) {
    /* Whole seconds since the anchor's minute, not wrapped: when the anchor is
     * not refreshed for a minute, a window 60 s on is a different window, and
     * its start (and symbol timestamp) must be 60 s on too. */
    return (int)floorf((timestamp_ms - anchor_ms + WINDOW_LEAD_MS) / WINDOW_DURATION_MS);
}

int bcd_window_second_of_index(int index) {
    return ((index % 60) + 60) % 60;
}

int bcd_window_get_second_for_timestamp(bcd_correlator_t *corr, float timestamp_ms, float anchor_ms) {
    if (anchor_ms < 0) return -1;
    return bcd_window_second_of_index(bcd_window_get_index(timestamp_ms, anchor_ms));
}

float bcd_window_get_start(float anchor_ms, int index) {
    return anchor_ms + (index * WINDOW_DURATION_MS) - WINDOW_LEAD_MS;
}

/*============================================================================
 * Window Management Functions
 *============================================================================*/

void bcd_window_open(bcd_correlator_t *corr, int index, float anchor_ms) {
    corr->window_open = true;
    corr->window_index = index;
    corr->current_second = bcd_window_second_of_index(index);
    corr->window_start_ms = bcd_window_get_start(anchor_ms, index);
    corr->window_anchor_ms = anchor_ms;
    corr->window_partial = false;
    corr->window_epoch = corr->sync_source ? sync_detector_get_phase_epoch(corr->sync_source) : 0;
    corr->window_resets = corr->sync_source ? sync_detector_get_phase_resets(corr->sync_source) : 0;

    /* Reset accumulators */
    corr->time_energy_sum = 0.0f;
    corr->time_duration_sum = 0.0f;
    corr->time_event_count = 0;
    corr->time_first_ms = 0.0f;
    corr->time_first_dur_ms = 0.0f;
    corr->time_last_ms = 0.0f;

    corr->freq_energy_sum = 0.0f;
    corr->freq_duration_sum = 0.0f;
    corr->freq_event_count = 0;
    corr->freq_first_ms = 0.0f;
    corr->freq_first_dur_ms = 0.0f;
    corr->freq_last_ms = 0.0f;

    /* A trailing edge noted before this window opened */
    corr->edge_valid = false;
    if (corr->edge_pending) {
        if (corr->edge_pending_end_ms < corr->window_start_ms) {
            corr->edge_pending = false;                 /* its window was never opened */
        } else if (corr->edge_pending_end_ms < corr->window_start_ms + WINDOW_DURATION_MS) {
            corr->edge_valid = true;
            corr->edge_start_ms = corr->edge_pending_start_ms;
            corr->edge_end_ms = corr->edge_pending_end_ms;
            corr->edge_pending = false;
        }
    }
}

void bcd_window_note_edge(bcd_correlator_t *corr, float start_ms, float end_ms) {
    float anchor_ms = bcd_window_get_minute_anchor(corr);
    if (anchor_ms < 0) return;
    int si = bcd_window_get_index(start_ms, anchor_ms);
    int ei = bcd_window_get_index(end_ms, anchor_ms);
    if (ei <= si) return;                       /* ends in the window it started in */
    if (corr->window_open && ei == corr->window_index) {
        corr->edge_valid = true;
        corr->edge_start_ms = start_ms;
        corr->edge_end_ms = end_ms;
    } else if (!corr->window_open || ei > corr->window_index) {
        corr->edge_pending = true;
        corr->edge_pending_start_ms = start_ms;
        corr->edge_pending_end_ms = end_ms;
    }                                           /* else: that second is already emitted */
}

void bcd_window_close(bcd_correlator_t *corr) {
    if (!corr->window_open) return;

    /* The trailing edge of this second's pulse, from an event that started in an
     * earlier window (see EDGE_MIN_END_MS). It gives the symbol when no event of
     * this window starts before it. */
    float second_ms = corr->window_start_ms + WINDOW_LEAD_MS;
    bool edge = corr->edge_valid && corr->edge_end_ms - second_ms >= EDGE_MIN_END_MS &&
                !(corr->time_event_count > 0 && corr->time_first_ms < corr->edge_end_ms) &&
                !(corr->freq_event_count > 0 && corr->freq_first_ms < corr->edge_end_ms);
    float edge_width_ms = corr->edge_end_ms - second_ms - EDGE_LEAD_MS;
    if (edge && bcd_symbol_classify_duration(edge_width_ms, corr->current_second) == BCD_CORR_SYM_NONE)
        edge = false;

    /* A window with no event that sync did not cover from its start says
     * nothing about its second: it is not emitted. */
    if (corr->window_partial && corr->time_event_count + corr->freq_event_count == 0 && !edge) {
        corr->window_open = false;
        return;
    }
    if (corr->time_event_count + corr->freq_event_count == 0 && !edge) corr->silent_windows++;

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

    /* Where the window's first event starts, from the start of its second. A
     * BCD pulse starts at the second; an event that starts mid-second is not
     * that second's pulse. NAN when the window holds no event. */
    float pulse_offset_ms = NAN;
    if (corr->time_event_count > 0) pulse_offset_ms = corr->time_first_ms;
    if (corr->freq_event_count > 0 &&
        (isnan(pulse_offset_ms) || corr->freq_first_ms < pulse_offset_ms))
        pulse_offset_ms = corr->freq_first_ms;
    pulse_offset_ms -= corr->window_start_ms + WINDOW_LEAD_MS;

    /* The pulse ran through the second inside an event that started earlier:
     * its width is the trailing edge's, and it started at that event's start,
     * before the second. */
    if (edge) {
        duration_ms = edge_width_ms;
        symbol = bcd_symbol_classify_duration(duration_ms, corr->current_second);
        pulse_offset_ms = corr->edge_start_ms - second_ms;
        source = "EDGE";
        if (confidence == 0.0f) confidence = 0.6f;
    }

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
    telem_sendf(TELEM_BCDS, "SYMBOL,%d,%.1f,%c,%.0f,%d,%d,%s,%.2f,%d,%.0f",
                corr->symbol_count, symbol_timestamp_ms,
                bcd_corr_symbol_char(symbol), duration_ms,
                corr->time_event_count, corr->freq_event_count, source, confidence,
                corr->current_second, pulse_offset_ms);

    /* Callback */
    if (corr->callback) {
        bcd_symbol_event_t event = {
            .symbol = symbol,
            .timestamp_ms = symbol_timestamp_ms,
            .duration_ms = duration_ms,
            .confidence = confidence,
            .source = source,
            .second = corr->current_second,
            .minute_start_ms = corr->window_anchor_ms +
                               60000.0f * (float)((corr->window_index - corr->current_second) / 60),
            .pulse_offset_ms = pulse_offset_ms,
            .phase_epoch = corr->window_epoch,
            .phase_resets = corr->window_resets
        };
        corr->callback(&event, corr->callback_user_data);
    }

    /* Close window */
    corr->window_open = false;
}

/* The held phase broke (moved 100 ms or more, or was lost) while the window
 * was open: its events were binned under two phases, so it is not emitted.
 * Trailing edges noted under the old phase go with it. */
static void discard_on_break(bcd_correlator_t *corr) {
    if (!corr->window_open || !corr->sync_source) return;
    int epoch = sync_detector_get_phase_epoch(corr->sync_source);
    if (epoch == corr->window_epoch) return;
    printf("[BCD] Sec %02d discarded: sync phase break inside the window\n", corr->current_second);
    corr->window_open = false;
    corr->edge_valid = false;
    corr->edge_pending = false;
    corr->broken_windows++;
}

/* Follow the sync anchor. When it moved by a whole number of minutes (within
 * half a window), the open window is the same second under both anchors:
 * re-index it and keep its events. The next minute's marker usually confirms
 * during second 1-2, while that second's window is open under the old anchor
 * (index 61); closing it and opening another would emit that second twice.
 * An anchor at a different phase changes the second numbering, so the open
 * window is closed. */
static void follow_anchor(bcd_correlator_t *corr, float anchor_ms) {
    if (!corr->window_open || anchor_ms == corr->window_anchor_ms) return;
    float delta = anchor_ms - corr->window_anchor_ms;
    float minutes = roundf(delta / 60000.0f);
    if (fabsf(delta - minutes * 60000.0f) < WINDOW_DURATION_MS / 2.0f) {
        corr->window_index -= 60 * (int)minutes;
        corr->window_anchor_ms = anchor_ms;
    } else {
        bcd_window_close(corr);
    }
}

bool bcd_window_check_transition(bcd_correlator_t *corr, float timestamp_ms) {
    if (!corr) return false;

    discard_on_break(corr);

    /* Get current sync state */
    float anchor_ms = bcd_window_get_minute_anchor(corr);

    /* If sync not locked, close any open window. Lock did not cover it to its
     * end, so it is sent only if it holds an event. */
    if (anchor_ms < 0) {
        if (corr->window_open) {
            corr->window_partial = true;
            bcd_window_close(corr);
        }
        return false;
    }

    follow_anchor(corr, anchor_ms);

    /* Determine which window this event belongs to */
    int event_index = bcd_window_get_index(timestamp_ms, anchor_ms);

    /* No window open: lock (or a new phase) began during this one */
    if (!corr->window_open) {
        bcd_window_open(corr, event_index, anchor_ms);
        corr->window_partial = true;
        return true;
    }

    if (event_index == corr->window_index) return true;

    if (event_index < corr->window_index) {
        /* An event for a window already passed. If the open window is the
         * partial first one and still empty, lock began before this event's
         * window: start there instead. Otherwise that second is already
         * emitted, and the event is dropped. */
        if (corr->window_partial && corr->time_event_count + corr->freq_event_count == 0) {
            bcd_window_open(corr, event_index, anchor_ms);
            corr->window_partial = true;
            return true;
        }
        corr->late_events++;
        return false;
    }

    /* A later second: close the current window, open the event's. A skipped
     * window that holds a trailing edge is opened and closed on the way. */
    int from = corr->window_index;
    bcd_window_close(corr);
    if (corr->edge_pending) {
        int ei = bcd_window_get_index(corr->edge_pending_end_ms, anchor_ms);
        if (ei > from && ei < event_index) {
            bcd_window_open(corr, ei, anchor_ms);
            bcd_window_close(corr);
        }
    }
    bcd_window_open(corr, event_index, anchor_ms);
    return true;
}

void bcd_window_periodic(bcd_correlator_t *corr, float now_ms) {
    if (!corr) return;
    discard_on_break(corr);
    float anchor_ms = bcd_window_get_minute_anchor(corr);
    if (anchor_ms < 0) {
        if (corr->window_open) {
            corr->window_partial = true;   /* lock ended inside it */
            bcd_window_close(corr);
        }
        return;
    }
    follow_anchor(corr, anchor_ms);
    if (!corr->window_open) {
        /* Lock (or a new phase) began during this window: it is partial */
        bcd_window_open(corr, bcd_window_get_index(now_ms, anchor_ms), anchor_ms);
        corr->window_partial = true;
        return;
    }
    /* Every event for a window arrives by its end + WINDOW_CLOSE_LATENCY_MS
     * (BTIME and BFREQ report at pulse end and cap a pulse at 1100 / 1000 ms).
     * Past that, the window is complete: close it, and open the next second's. A window that ends with
     * no event while sync was LOCKED throughout is emitted as NONE. */
    while (now_ms >= corr->window_start_ms + WINDOW_DURATION_MS + WINDOW_CLOSE_LATENCY_MS) {
        int next = corr->window_index + 1;
        bcd_window_close(corr);
        bcd_window_open(corr, next, anchor_ms);
    }
}
