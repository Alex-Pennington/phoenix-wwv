/**
 * @file detector_routing.c
 * @brief Internal event routing callbacks
 *
 * Routes detector events to correlators and external callbacks.
 */

#include "wwv_detector_manager_internal.h"

/*============================================================================
 * Tick Event Routing
 *============================================================================*/

void wwv_routing_on_tick_event(const tick_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
    /* NOTE: tick_correlator requires full tick information (12 parameters)
     * which is only available in tick_detector internals. The correlator
     * should be fed directly from tick_detector, not through the manager.
     * Keeping this routing for future implementation. */
    
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
    
    /* Feed sync detector */
    if (mgr->sync_detector) {
        sync_detector_tick_marker(mgr->sync_detector,
                                   event->timestamp_ms,
                                   event->duration_ms,
                                   event->corr_ratio);
    }
}

/*============================================================================
 * Marker Event Routing
 *============================================================================*/

void wwv_routing_on_marker_event(const marker_event_t *event, void *user_data) {
    wwv_detector_manager_t *mgr = (wwv_detector_manager_t *)user_data;
    
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
