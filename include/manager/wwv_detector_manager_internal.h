/**
 * @file wwv_detector_manager_internal.h
 * @brief Internal structures and declarations for detector manager
 */

#ifndef WWV_DETECTOR_MANAGER_INTERNAL_H
#define WWV_DETECTOR_MANAGER_INTERNAL_H

#include "wwv_detector_manager.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"
#include "tone_tracker.h"
#include "tick_correlator.h"
#include "marker_correlator.h"
#include "slow_marker_detector.h"
#include <stdint.h>

/*============================================================================
 * Internal State Structure
 *============================================================================*/

struct wwv_detector_manager {
    /* Detector path (50 kHz) */
    tick_detector_t *tick_detector;
    marker_detector_t *marker_detector;
    
    /* Correlators */
    tick_correlator_t *tick_correlator;
    marker_correlator_t *marker_correlator;
    sync_detector_t *sync_detector;
    
    /* Display path (12 kHz) */
    tone_tracker_t *tone_carrier;
    tone_tracker_t *tone_500;
    tone_tracker_t *tone_600;
    slow_marker_detector_t *slow_marker;
    
    /* External callbacks */
    wwv_tick_callback_fn tick_callback;
    void *tick_callback_data;
    wwv_marker_callback_fn marker_callback;
    void *marker_callback_data;
    wwv_sync_callback_fn sync_callback;
    void *sync_callback_data;
    
    /* Statistics */
    uint64_t detector_samples;
    uint64_t display_samples;
};

/*============================================================================
 * Lifecycle Functions
 *============================================================================*/

/**
 * Create all detector components based on configuration
 * @param mgr Pre-allocated manager structure (zeroed)
 * @param config Configuration specifying which detectors to enable
 * @return true on success, false on failure
 */
bool wwv_detector_lifecycle_create_all(wwv_detector_manager_t *mgr,
                                        const wwv_detector_config_t *config);

/**
 * Destroy all detector components and free resources
 * @param mgr Manager instance
 */
void wwv_detector_lifecycle_destroy_all(wwv_detector_manager_t *mgr);

/*============================================================================
 * Routing Callback Functions
 *============================================================================*/

/**
 * Internal callback for tick events
 * Routes to tick correlator and external callback
 */
void wwv_routing_on_tick_event(const tick_event_t *event, void *user_data);

/**
 * Internal callback for tick marker events
 * Routes to sync detector
 */
void wwv_routing_on_tick_marker_event(const tick_marker_event_t *event, void *user_data);

/**
 * Internal callback for marker events
 * Routes to marker correlator and external callback
 */
void wwv_routing_on_marker_event(const marker_event_t *event, void *user_data);

/**
 * Internal callback for slow marker frames
 * Routes to marker correlator for verification
 */
void wwv_routing_on_slow_marker_frame(const slow_marker_frame_t *frame, void *user_data);

#endif /* WWV_DETECTOR_MANAGER_INTERNAL_H */
