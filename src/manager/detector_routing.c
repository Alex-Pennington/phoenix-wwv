/**
 * @file detector_routing.c
 * @brief Internal event routing callbacks
 *
 * Routes detector events to the sync detector, the correlators, the time decoder,
 * the observer and the external callbacks. This is the chain's only wiring:
 * tests/air/harness_iq.c and the hosts use it rather than wiring detectors
 * themselves.
 */

#include "wwv_detector_manager_internal.h"

/*============================================================================
 * Tick Event Routing
 *============================================================================*/

void wwv_routing_on_tick_event(const tick_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_tick) mgr->observer.on_tick(event, mgr->observer.user);

    /* Ticks are not fed to the sync detector (sync_detector_tick_event): the chain
     * scored on air never fed them. tick_correlator requires full tick information
     * (12 parameters) which is only available in tick_detector internals. */

    /* Forward to external callback */
    if (mgr->tick_callback) {
        wwv_tick_event_t ext_event = {
            .tick_number = event->tick_number,
            .timestamp_ms = event->timestamp_ms,
            .duration_ms = event->duration_ms,
            .energy = event->peak_energy
        };
        mgr->tick_callback(&ext_event, mgr->tick_callback_data);
    }
}

/*============================================================================
 * Tick Marker Event Routing
 *============================================================================*/

void wwv_routing_on_tick_marker_event(const tick_marker_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_tick_marker) mgr->observer.on_tick_marker(event, mgr->observer.user);

    /* Feed sync detector: it takes the marker's leading edge, not timestamp_ms
     * (the trailing edge, ~0.8 s later) */
    if (mgr->sync_detector) {
        sync_detector_tick_marker(mgr->sync_detector,
                                   event->start_timestamp_ms,
                                   event->duration_ms,
                                   event->corr_ratio);
    }

    /* First marker sets the tick detector's timing epoch */
    if (mgr->tick_detector &&
        tick_detector_get_epoch_source(mgr->tick_detector) == EPOCH_SOURCE_NONE) {
        tick_detector_set_epoch_with_source(mgr->tick_detector, event->start_timestamp_ms,
                                            EPOCH_SOURCE_MARKER, 0.7f);
    }
}

/*============================================================================
 * Marker Event Routing
 *============================================================================*/

void wwv_routing_on_marker_event(const marker_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_marker) mgr->observer.on_marker(event, mgr->observer.user);

    /* Feed sync detector with the leading edge */
    if (mgr->sync_detector) {
        sync_detector_marker_event(mgr->sync_detector,
                                   event->start_timestamp_ms,
                                   event->accumulated_energy,
                                   event->duration_ms);
    }

    /* Feed correlator */
    if (mgr->marker_correlator) {
        marker_correlator_fast_event(mgr->marker_correlator,
                                      event->timestamp_ms,
                                      event->duration_ms);
    }

    /* Forward to external callback */
    if (mgr->marker_callback) {
        wwv_marker_event_t ext_event = {
            .marker_number = event->marker_number,
            .timestamp_ms = event->timestamp_ms,
            .since_last_sec = event->since_last_marker_sec,
            .duration_ms = event->duration_ms,
            .energy = event->accumulated_energy
        };
        mgr->marker_callback(&ext_event, mgr->marker_callback_data);
    }
}

/*============================================================================
 * Slow Marker Frame Routing
 *============================================================================*/

void wwv_routing_on_slow_marker_frame(const slow_marker_frame_t *frame, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    /* Feed correlator for verification */
    if (mgr->marker_correlator) {
        marker_correlator_slow_frame(mgr->marker_correlator,
                                      frame->timestamp_ms,
                                      frame->energy,
                                      frame->snr_db,
                                      frame->above_threshold);
    }

    /* NOTE: We do NOT inject slow_marker's baseline into marker_detector!
     * The FFT configurations are incompatible (12kHz/2048 vs 50kHz/256).
     * Each detector tracks its own baseline independently.
     */
}

/*============================================================================
 * BCD Routing
 *============================================================================*/

void wwv_routing_on_bcd_time_event(const bcd_time_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_bcd_time) mgr->observer.on_bcd_time(event, mgr->observer.user);
    if (mgr->bcd_correlator) {
        bcd_correlator_time_event(mgr->bcd_correlator, event->timestamp_ms,
                                  event->duration_ms, event->peak_energy);
    }
    if (mgr->observer.on_bcd_time_routed) mgr->observer.on_bcd_time_routed(event, mgr->observer.user);
}

void wwv_routing_on_bcd_freq_event(const bcd_freq_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_bcd_freq) mgr->observer.on_bcd_freq(event, mgr->observer.user);
    if (mgr->bcd_correlator) {
        bcd_correlator_freq_event(mgr->bcd_correlator, event->timestamp_ms,
                                  event->duration_ms, event->accumulated_energy);
    }
    if (mgr->observer.on_bcd_freq_routed) mgr->observer.on_bcd_freq_routed(event, mgr->observer.user);
}

void wwv_routing_on_bcd_symbol(const bcd_symbol_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_symbol) mgr->observer.on_symbol(event, mgr->observer.user);
    if (mgr->time_decoder) wwv_time_decoder_bcd_callback(event, mgr->time_decoder);
}

void wwv_routing_on_time_result(const wwv_td_result_t *result, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_time) mgr->observer.on_time(result, mgr->observer.user);
}

/*============================================================================
 * Sync State Routing
 *============================================================================*/

void wwv_routing_on_sync_state(sync_state_t old_state, sync_state_t new_state,
                               float confidence, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;

    if (mgr->observer.on_sync_state) {
        mgr->observer.on_sync_state(old_state, new_state, confidence, mgr->observer.user);
    }
    if (mgr->sync_callback) {
        wwv_sync_status_t status = wwv_detector_manager_get_sync_status(mgr);
        mgr->sync_callback(&status, mgr->sync_callback_data);
    }
}
