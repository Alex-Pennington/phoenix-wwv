/**
 * @file bcd_correlator.c
 * @brief WWV BCD Window-Based Symbol Demodulator
 *
 * ARCHITECTURE (v2 - Window-Based):
 *   - Gates on sync_detector LOCKED state
 *   - Uses minute anchor to define 1-second windows
 *   - Integrates energy from time/freq detectors over each window
 *   - Classifies ONCE per window at window close
 *   - Emits exactly 60 symbols per minute (one per second)
 *
 * Signal Flow:
 *   sync_detector (LOCKED) provides anchor_ms
 *   → second boundaries: anchor + 0s, anchor + 1s, ... anchor + 59s
 *   → time_detector events accumulate energy in current window
 *   → freq_detector events accumulate energy in current window
 *   → at window close: integrate, classify, emit ONE symbol
 *
 * Pattern: Follows sync_detector.c state machine structure
 */

#include "bcd_correlator.h"
#include "bcd_correlator_internal.h"
#include "sync_detector.h"
#include "version.h"
#include "telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/*============================================================================
 * Public API
 *============================================================================*/

bcd_correlator_t *bcd_correlator_create(const char *csv_path) {
    bcd_correlator_t *corr = (bcd_correlator_t *)calloc(1, sizeof(bcd_correlator_t));
    if (!corr) return NULL;

    corr->state = BCD_CORR_ACQUIRING;
    corr->start_time = time(NULL);
    corr->time_prev_end_ms = NAN;
    corr->window_open = false;

    if (csv_path) {
        corr->csv_file = fopen(csv_path, "w");
        if (corr->csv_file) {
            char time_str[64];
            time_t now = time(NULL);
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(corr->csv_file, "# Phoenix SDR BCD Correlator Log v%s\n", PHOENIX_VERSION_FULL);
            fprintf(corr->csv_file, "# Started: %s\n", time_str);
            fprintf(corr->csv_file, "# Window-based integration: 1-second windows gated on sync LOCKED\n");
            fprintf(corr->csv_file, "time,timestamp_ms,symbol_num,second,symbol,source,duration_ms,confidence,interval_sec,time_events,freq_events,time_energy,freq_energy,state\n");
            fflush(corr->csv_file);
        }
    }

    printf("[BCD] Window-based correlator created (waits for sync LOCKED)\n");

    return corr;
}

void bcd_correlator_destroy(bcd_correlator_t *corr) {
    if (!corr) return;

    /* The last window ends with the stream, before its close latency: send it
     * as a normal window close does, and only if it holds an event (partial) */
    if (corr->window_open) {
        corr->window_partial = true;
        bcd_window_close(corr);
    }

    if (corr->csv_file) fclose(corr->csv_file);
    free(corr);
}

void bcd_correlator_set_sync_source(bcd_correlator_t *corr, sync_detector_t *sync) {
    if (!corr) return;
    corr->sync_source = sync;
    printf("[BCD] Sync source linked - will gate on LOCKED state\n");
}

void bcd_correlator_set_callback(bcd_correlator_t *corr,
                                 bcd_corr_symbol_callback_fn callback,
                                 void *user_data) {
    if (!corr) return;
    corr->callback = callback;
    corr->callback_user_data = user_data;
}

void bcd_correlator_time_event(bcd_correlator_t *corr,
                               wwv_time_ms_t timestamp_ms,
                               float duration_ms,

                               float peak_energy) {
    if (!corr) return;

    /* Check for window transition first; ignore the event when sync is not
     * locked or its window was already emitted */
    bool accepted = bcd_window_check_transition(corr, timestamp_ms);

    /* Its end may be a later window's trailing edge, even when its own window
     * is already emitted */
    bcd_window_note_edge(corr, timestamp_ms, timestamp_ms + duration_ms);
    wwv_time_ms_t prev_end_ms = corr->time_prev_end_ms;
    corr->time_prev_end_ms = timestamp_ms + duration_ms;
    if (!accepted) return;

    /* Accumulate into current window */
    if (corr->time_event_count == 0) {
        corr->time_first_ms = timestamp_ms;
        corr->time_first_dur_ms = duration_ms;
        corr->time_first_prev_end_ms = prev_end_ms;
    }
    corr->time_last_ms = timestamp_ms;
    corr->time_energy_sum += peak_energy;
    corr->time_duration_sum += duration_ms;
    corr->time_event_count++;
}

void bcd_correlator_freq_event(bcd_correlator_t *corr,
                               wwv_time_ms_t timestamp_ms,
                               float duration_ms,

                               float accum_energy) {
    if (!corr) return;

    /* Check for window transition first; ignore the event when sync is not
     * locked or its window was already emitted */
    if (!bcd_window_check_transition(corr, timestamp_ms)) return;

    /* Accumulate into current window */
    if (corr->freq_event_count == 0) {
        corr->freq_first_ms = timestamp_ms;
        corr->freq_first_dur_ms = duration_ms;
    }
    corr->freq_last_ms = timestamp_ms;
    corr->freq_energy_sum += accum_energy;
    corr->freq_duration_sum += duration_ms;
    corr->freq_event_count++;
}

void bcd_correlator_periodic_check(bcd_correlator_t *corr, wwv_time_ms_t now_ms) {
    bcd_window_periodic(corr, now_ms);
}

bcd_corr_state_t bcd_correlator_get_state(bcd_correlator_t *corr) {
    return corr ? corr->state : BCD_CORR_ACQUIRING;
}

const char *bcd_corr_state_name(bcd_corr_state_t state) {
    switch (state) {
        case BCD_CORR_ACQUIRING: return "ACQUIRING";
        case BCD_CORR_TENTATIVE: return "TENTATIVE";
        case BCD_CORR_TRACKING:  return "TRACKING";
        default:                 return "UNKNOWN";
    }
}

char bcd_corr_symbol_char(bcd_corr_symbol_t sym) {
    switch (sym) {
        case BCD_CORR_SYM_ZERO:   return '0';
        case BCD_CORR_SYM_ONE:    return '1';
        case BCD_CORR_SYM_MARKER: return 'P';
        default:                  return '.';  /* No signal = dot */
    }
}

wwv_time_ms_t bcd_correlator_get_last_symbol_ms(bcd_correlator_t *corr) {
    return corr ? corr->last_symbol_ms : 0.0;

}

int bcd_correlator_get_symbol_count(bcd_correlator_t *corr) {
    return corr ? corr->symbol_count : 0;
}

void bcd_correlator_print_stats(bcd_correlator_t *corr) {
    if (!corr) return;

    printf("\n=== BCD CORRELATOR STATS ===\n");
    printf("Mode: Window-based (1-second integration)\n");
    printf("Sync source: %s\n", corr->sync_source ? "linked" : "NOT LINKED");
    printf("State: %s\n", bcd_corr_state_name(corr->state));
    printf("Symbols emitted: %d\n", corr->symbol_count);
    printf("Good intervals (~1s): %d\n", corr->good_intervals);
    printf("Last symbol at: %.1fms\n", corr->last_symbol_ms);
    printf("Current window: %s (second %d)\n",
           corr->window_open ? "OPEN" : "CLOSED", corr->current_second);
    printf("============================\n");
}
