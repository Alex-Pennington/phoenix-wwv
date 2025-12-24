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

/*============================================================================
 * Lifecycle
 *============================================================================*/

wwv_detector_manager_t *wwv_detector_manager_create(const wwv_detector_config_t *config) {
    wwv_detector_manager_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    
    if (!wwv_detector_lifecycle_create_all(mgr, config)) {
        free(mgr);
        return NULL;
    }
    
    return mgr;
}

void wwv_detector_manager_destroy(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    /* Print final stats before cleanup */
    wwv_detector_manager_print_stats(mgr);
    
    /* Destroy all detectors */
    wwv_detector_lifecycle_destroy_all(mgr);
    
    free(mgr);
}

/*============================================================================
 * Sample Processing
 *============================================================================*/

void wwv_detector_manager_process_detector_sample(wwv_detector_manager_t *mgr,
                                                   float i_sample, float q_sample) {
    if (!mgr) return;
    
    if (mgr->tick_detector) {
        tick_detector_process_sample(mgr->tick_detector, i_sample, q_sample);
    }
    
    if (mgr->marker_detector) {
        marker_detector_process_sample(mgr->marker_detector, i_sample, q_sample);
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
 * Callbacks
 *============================================================================*/

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
        status.confidence = sync_detector_get_confidence(mgr->sync_detector);
        status.drift_ppm = 0.0f;  /* TODO: sync_detector doesn't provide drift yet */
    }
    
    if (mgr) {
        status.tick_count = wwv_detector_manager_get_tick_count(mgr);
        status.marker_count = wwv_detector_manager_get_marker_count(mgr);
    }
    
    return status;
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
