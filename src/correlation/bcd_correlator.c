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
 * Public API - Window-Based Symbol Demodulator
 *============================================================================*/

static void close_window(bcd_correlator_t *corr) {
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

    /* Log to CSV and telemetry */
    char time_str[16];
    bcd_corr_get_wall_time_str(corr->start_time, symbol_timestamp_ms, time_str, sizeof(time_str));

    if (corr->csv_file) {
        fprintf(corr->csv_file, "%s,%.1f,%d,%d,%c,%s,%.0f,%.2f,%.1f,%d,%d,%.4f,%.4f,%s\n",
                time_str, symbol_timestamp_ms, corr->symbol_count, corr->current_second,
                bcd_corr_symbol_char(symbol), source,
                duration_ms, confidence, interval_ms / 1000.0f,
                corr->time_event_count, corr->freq_event_count,
                corr->time_energy_sum, corr->freq_energy_sum,
                bcd_corr_state_name(corr->state));
        fflush(corr->csv_file);
    }

    /* UDP telemetry for correlation stats */
    telem_sendf(TELEM_BCDS, "CORR,%s,%.1f,%d,%d,%c,%s,%.0f,%.2f,%.1f,%d,%d,%.4f,%.4f,%s",
                time_str, symbol_timestamp_ms, corr->symbol_count, corr->current_second,
                bcd_corr_symbol_char(symbol), source,
                duration_ms, confidence, interval_ms / 1000.0f,
                corr->time_event_count, corr->freq_event_count,
                corr->time_energy_sum, corr->freq_energy_sum,
                bcd_corr_state_name(corr->state));

    /* Only emit if we detected something */
    if (symbol != BCD_CORR_SYM_NONE) {
        /* Step 9: UDP telemetry with second position and confidence */
        telem_sendf(TELEM_BCDS, "SYM,%c,%d,%.0f,%.2f",
                    bcd_corr_symbol_char(symbol),
                    corr->current_second,
                    duration_ms,
                    confidence);

        /* Console output (verbose during development) */
        printf("[BCD] Sec %02d: '%c' dur=%.0fms conf=%.2f src=%s events=%d+%d state=%s\n",
               corr->current_second, bcd_corr_symbol_char(symbol),
               duration_ms, confidence, source,
               corr->time_event_count, corr->freq_event_count,
               bcd_corr_state_name(corr->state));

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
    }

    /* Mark window closed */
    corr->window_open = false;
}

/*============================================================================
 * Public API
 *============================================================================*/

bcd_correlator_t *bcd_correlator_create(const char *csv_path) {
    bcd_correlator_t *corr = (bcd_correlator_t *)calloc(1, sizeof(bcd_correlator_t));
    if (!corr) return NULL;

    corr->state = BCD_CORR_ACQUIRING;
    corr->start_time = time(NULL);
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

    /* Close any open window */
    if (corr->window_open) {
        close_window(corr);
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
                               float timestamp_ms,
                               float duration_ms,
                               float peak_energy) {
    if (!corr) return;

    /* Check for window transition first */
    bcd_window_check_transition(corr, timestamp_ms);

    /* If no window open (sync not locked), ignore event */
    if (!corr->window_open) return;

    /* Accumulate into current window */
    if (corr->time_event_count == 0) {
        corr->time_first_ms = timestamp_ms;
    }
    corr->time_last_ms = timestamp_ms;
    corr->time_energy_sum += peak_energy;
    corr->time_duration_sum += duration_ms;
    corr->time_event_count++;
}

void bcd_correlator_freq_event(bcd_correlator_t *corr,
                               float timestamp_ms,
                               float duration_ms,
                               float accum_energy) {
    if (!corr) return;

    /* Check for window transition first */
    bcd_window_check_transition(corr, timestamp_ms);

    /* If no window open (sync not locked), ignore event */
    if (!corr->window_open) return;

    /* Accumulate into current window */
    if (corr->freq_event_count == 0) {
        corr->freq_first_ms = timestamp_ms;
    }
    corr->freq_last_ms = timestamp_ms;
    corr->freq_energy_sum += accum_energy;
    corr->freq_duration_sum += duration_ms;
    corr->freq_event_count++;
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

float bcd_correlator_get_last_symbol_ms(bcd_correlator_t *corr) {
    return corr ? corr->last_symbol_ms : 0.0f;
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
