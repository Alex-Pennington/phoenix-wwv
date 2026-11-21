/**
 * @file wwv_detector_manager.c
 * @brief Centralized WWV detector orchestration implementation
 *
 * See wwv_detector_manager.h for architecture documentation.
 */

#include "wwv_detector_manager.h"
#include "wwv_detector_manager_internal.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"
#include "tone_tracker.h"
#include "slow_marker_detector.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/*============================================================================
 * Lifecycle
 *============================================================================*/

wwv_detector_manager_t *wwv_detector_manager_create(const wwv_detector_config_t *config) {
    wwv_detector_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    
    if (!wwv_detector_lifecycle_create_all(mgr, config)) {
        wwv_detector_lifecycle_destroy_all(mgr);
        free(mgr);
        return NULL;
    }
    
    return mgr;
}

void wwv_detector_manager_destroy(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    /* Print final stats before cleanup */
    if (mgr->verbose) wwv_detector_manager_print_stats(mgr);
    
    /* Destroy all detectors */
    wwv_detector_lifecycle_destroy_all(mgr);
    
    free(mgr);
}

/*============================================================================
 * Sample Processing
 *============================================================================*/

/* waterfall.c (1ec38d1) normalize(): fast warmup, then slow */
static float normalize(wwv_normalizer_t *n, float i, float q) {
    float mag = sqrtf(i*i + q*q);
    float alpha = (n->warmup < 50000) ? 0.01f : 0.0001f;
    n->level += alpha * (mag - n->level);
    n->warmup++;
    if (n->level < 0.0001f) n->level = 0.0001f;
    return 1.0f / n->level;
}

void wwv_detector_manager_process_detector_sample(wwv_detector_manager_t *mgr,
                                                   float i_sample, float q_sample) {
    if (!mgr) return;
    
    uint64_t n = mgr->detector_samples;
    mgr->now_ms = (double)n / (double)WWV_DETECTOR_SAMPLE_RATE * 1000.0;
    
    if (mgr->agc) {
        float nf = normalize(&mgr->normalizer, i_sample, q_sample);
        i_sample *= nf;
        q_sample *= nf;
    }
    
    /* Sync channel (800-1400 Hz) for ticks and markers, data channel (50-150 Hz)
     * for the BCD subcarrier */
    float s_i = sync_channel_process(&mgr->sync_i, i_sample);
    float s_q = sync_channel_process(&mgr->sync_q, q_sample);
    float d_i = data_channel_process(&mgr->data_i, i_sample);
    float d_q = data_channel_process(&mgr->data_q, q_sample);
    
    if (mgr->tick_detector) tick_detector_process_sample(mgr->tick_detector, s_i, s_q);
    if (mgr->marker_detector) marker_detector_process_sample(mgr->marker_detector, s_i, s_q);
    if (mgr->bcd_time) bcd_time_detector_process_sample(mgr->bcd_time, d_i, d_q);
    if (mgr->bcd_freq) bcd_freq_detector_process_sample(mgr->bcd_freq, d_i, d_q);
    
    if (n % WWV_PERIODIC_CHECK_SAMPLES == 0) {
        float now = (float)mgr->now_ms;
        if (mgr->sync_detector) sync_detector_periodic_check(mgr->sync_detector, now);
        if (mgr->bcd_correlator) bcd_correlator_periodic_check(mgr->bcd_correlator, now);
        if (mgr->time_decoder) wwv_time_decoder_periodic(mgr->time_decoder, now);
        /* Tick timing gate: on while sync is LOCKED, its epoch the held phase.
         * Needs the sync rule that ignores a candidate's start reading for a
         * whole-second loss: a gated run starts on a tick (NOTES.md, Detection). */
        if (mgr->sync_detector && mgr->tick_detector) {
            frame_time_t ft = sync_detector_get_frame_time(mgr->sync_detector);
            bool locked = (ft.state == SYNC_LOCKED);
            if (locked) {
                float ep = fmodf(ft.second_start_ms, 1000.0f);
                if (ep < 0.0f) ep += 1000.0f;
                if (fabsf(ep - tick_detector_get_epoch(mgr->tick_detector)) > 0.5f)
                    tick_detector_set_epoch_with_source(mgr->tick_detector, ep, EPOCH_SOURCE_MARKER, ft.confidence);
            }
            if (locked != tick_detector_is_gating_enabled(mgr->tick_detector))
                tick_detector_set_gating_enabled(mgr->tick_detector, locked);
        }
    }
    
    mgr->detector_samples++;
}

void wwv_detector_manager_process_display_sample(wwv_detector_manager_t *mgr,
                                                  float i_sample, float q_sample) {
    if (!mgr) return;
    
    if (mgr->tone_carrier) {
        tone_tracker_process_sample(mgr->tone_carrier, i_sample, q_sample);
    }
    if (mgr->tone_500) {
        tone_tracker_process_sample(mgr->tone_500, i_sample, q_sample);
    }
    if (mgr->tone_600) {
        tone_tracker_process_sample(mgr->tone_600, i_sample, q_sample);
    }
    
    mgr->display_samples++;
}

void wwv_detector_manager_process_display_fft(wwv_detector_manager_t *mgr,
                                               const kiss_fft_cpx *fft_out,
                                               float timestamp_ms) {
    if (!mgr || !fft_out) return;
    
    if (mgr->slow_marker) {
        slow_marker_detector_process_fft(mgr->slow_marker, fft_out, timestamp_ms);
    }
}

/*============================================================================
 * Filled gaps
 *============================================================================*/

void wwv_detector_manager_mark_gap(wwv_detector_manager_t *mgr, double start_ms, double len_ms) {
    if (!mgr || !(len_ms > 0.0)) return;
    double end_ms = start_ms + len_ms;
    if (mgr->n_gaps > 0) {
        /* a span that abuts or overlaps the newest one extends it */
        int last = (mgr->gap_next + WWV_GAP_SPANS - 1) % WWV_GAP_SPANS;
        if (start_ms <= mgr->gaps[last].end_ms + 1.0 && end_ms >= mgr->gaps[last].start_ms) {
            if (start_ms < mgr->gaps[last].start_ms) mgr->gaps[last].start_ms = start_ms;
            if (end_ms > mgr->gaps[last].end_ms) mgr->gaps[last].end_ms = end_ms;
            return;
        }
    }
    mgr->gaps[mgr->gap_next].start_ms = start_ms;
    mgr->gaps[mgr->gap_next].end_ms = end_ms;
    mgr->gap_next = (mgr->gap_next + 1) % WWV_GAP_SPANS;
    if (mgr->n_gaps < WWV_GAP_SPANS) mgr->n_gaps++;
}

bool wwv_gap_covers(const wwv_detector_manager_t *mgr, double lo_ms, double hi_ms) {
    if (!mgr) return false;
    if (hi_ms < lo_ms) { double t = lo_ms; lo_ms = hi_ms; hi_ms = t; }
    for (int k = 0; k < mgr->n_gaps; k++) {
        double len = mgr->gaps[k].end_ms - mgr->gaps[k].start_ms;
        double settle = WWV_GAP_SETTLE_MIN_MS + len;
        if (settle > WWV_GAP_SETTLE_MS) settle = WWV_GAP_SETTLE_MS;
        if (hi_ms >= mgr->gaps[k].start_ms - WWV_GAP_GUARD_MS &&
            lo_ms <= mgr->gaps[k].end_ms + settle)
            return true;
    }
    return false;
}

wwv_gap_drops_t wwv_detector_manager_get_gap_drops(wwv_detector_manager_t *mgr) {
    wwv_gap_drops_t z = {0};
    return mgr ? mgr->gap_drops : z;
}

/*============================================================================
 * Callbacks
 *============================================================================*/

void wwv_detector_manager_set_observer(wwv_detector_manager_t *mgr,
                                        const wwv_detector_observer_t *observer) {
    if (!mgr) return;
    if (observer) mgr->observer = *observer;
    else memset(&mgr->observer, 0, sizeof(mgr->observer));
}

void wwv_detector_manager_set_tick_callback(wwv_detector_manager_t *mgr,
                                             wwv_tick_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->tick_callback = cb;
    mgr->tick_callback_data = user_data;
}

void wwv_detector_manager_set_marker_callback(wwv_detector_manager_t *mgr,
                                               wwv_marker_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->marker_callback = cb;
    mgr->marker_callback_data = user_data;
}

void wwv_detector_manager_set_sync_callback(wwv_detector_manager_t *mgr,
                                             wwv_sync_callback_fn cb, void *user_data) {
    if (!mgr) return;
    mgr->sync_callback = cb;
    mgr->sync_callback_data = user_data;
}

/*============================================================================
 * Status / Diagnostics
 *============================================================================*/

wwv_sync_status_t wwv_detector_manager_get_sync_status(wwv_detector_manager_t *mgr) {
    wwv_sync_status_t status = {0};
    
    if (mgr && mgr->sync_detector) {
        sync_state_t state = sync_detector_get_state(mgr->sync_detector);
        status.is_synced = (state == SYNC_LOCKED);
        status.confidence = (int)(sync_detector_get_confidence(mgr->sync_detector) * 100.0f + 0.5f);
        status.drift_ppm = 0.0f;  /* TODO: sync_detector doesn't provide drift yet */
    }
    
    if (mgr) {
        status.tick_count = wwv_detector_manager_get_tick_count(mgr);
        status.marker_count = wwv_detector_manager_get_marker_count(mgr);
    }
    
    return status;
}

uint64_t wwv_detector_manager_get_detector_samples(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->detector_samples : 0;
}

double wwv_detector_manager_get_now_ms(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->now_ms : 0.0;
}

tick_detector_t *wwv_detector_manager_get_tick_detector(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->tick_detector : NULL;
}

marker_detector_t *wwv_detector_manager_get_marker_detector(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->marker_detector : NULL;
}

sync_detector_t *wwv_detector_manager_get_sync_detector(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->sync_detector : NULL;
}

bcd_correlator_t *wwv_detector_manager_get_bcd_correlator(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->bcd_correlator : NULL;
}

wwv_time_decoder_t *wwv_detector_manager_get_time_decoder(wwv_detector_manager_t *mgr) {
    return mgr ? mgr->time_decoder : NULL;
}

int wwv_detector_manager_get_tick_count(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->tick_detector) ? tick_detector_get_tick_count(mgr->tick_detector) : 0;
}

int wwv_detector_manager_get_marker_count(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->marker_detector) ? marker_detector_get_marker_count(mgr->marker_detector) : 0;
}

int wwv_detector_manager_get_tick_flash(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->tick_detector) ? tick_detector_get_flash_frames(mgr->tick_detector) : 0;
}

int wwv_detector_manager_get_marker_flash(wwv_detector_manager_t *mgr) {
    return (mgr && mgr->marker_detector) ? marker_detector_get_flash_frames(mgr->marker_detector) : 0;
}

void wwv_detector_manager_decrement_flash(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    if (mgr->tick_detector) tick_detector_decrement_flash(mgr->tick_detector);
    if (mgr->marker_detector) marker_detector_decrement_flash(mgr->marker_detector);
}

void wwv_detector_manager_log_metadata(wwv_detector_manager_t *mgr,
                                        uint64_t center_freq,
                                        uint32_t sample_rate,
                                        uint32_t gain_reduction,
                                        uint32_t lna_state) {
    if (!mgr) return;
    
    if (mgr->marker_detector) {
        marker_detector_log_metadata(mgr->marker_detector, center_freq,
                                      sample_rate, gain_reduction, lna_state);
    }
}

void wwv_detector_manager_log_display_gain(wwv_detector_manager_t *mgr, float display_gain) {
    if (!mgr) return;
    
    if (mgr->marker_detector) {
        marker_detector_log_display_gain(mgr->marker_detector, display_gain);
    }
}

void wwv_detector_manager_print_stats(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    printf("\n");
    printf("================================================================================\n");
    printf("                        WWV DETECTOR MANAGER STATS\n");
    printf("================================================================================\n");
    printf("Samples processed: detector=%llu display=%llu\n",
           (unsigned long long)mgr->detector_samples,
           (unsigned long long)mgr->display_samples);
    printf("\n");
    
    if (mgr->tick_detector) {
        tick_detector_print_stats(mgr->tick_detector);
    }
    
    if (mgr->marker_detector) {
        marker_detector_print_stats(mgr->marker_detector);
    }
    
    /* NOTE: sync_detector doesn't have print_stats yet */
    
    printf("================================================================================\n");
}
