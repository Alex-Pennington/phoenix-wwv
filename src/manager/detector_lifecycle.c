/**
 * @file detector_lifecycle.c
 * @brief Detector component lifecycle management
 *
 * Handles creation and destruction of all detector components
 * based on configuration.
 */

#include "wwv_detector_manager_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*============================================================================
 * Creation
 *============================================================================*/

bool wwv_detector_lifecycle_create_all(wwv_detector_manager_t *mgr,
                                        const wwv_detector_config_t *config) {
    char path[512];
    
    printf("\n[DETECTOR_MGR] Creating WWV detector manager...\n");
    
    /* Detector path components */
    if (config->enable_tick_detector) {
        snprintf(path, sizeof(path), "%s/wwv_ticks.csv", config->output_dir);
        mgr->tick_detector = tick_detector_create(path);
        if (mgr->tick_detector) {
            tick_detector_set_callback(mgr->tick_detector, wwv_routing_on_tick_event, mgr);
            tick_detector_set_marker_callback(mgr->tick_detector, wwv_routing_on_tick_marker_event, mgr);
        }
    }
    
    if (config->enable_marker_detector) {
        snprintf(path, sizeof(path), "%s/wwv_markers.csv", config->output_dir);
        mgr->marker_detector = marker_detector_create(path);
        if (mgr->marker_detector) {
            marker_detector_set_callback(mgr->marker_detector, wwv_routing_on_marker_event, mgr);
        }
    }
    
    /* Correlators */
    if (config->enable_correlators) {
        snprintf(path, sizeof(path), "%s/wwv_tick_corr.csv", config->output_dir);
        mgr->tick_correlator = tick_correlator_create(path);
        
        snprintf(path, sizeof(path), "%s/wwv_markers_corr.csv", config->output_dir);
        mgr->marker_correlator = marker_correlator_create(path);
    }
    
    if (config->enable_sync_detector) {
        snprintf(path, sizeof(path), "%s/wwv_sync.csv", config->output_dir);
        mgr->sync_detector = sync_detector_create(path);
    }
    
    /* Display path components */
    if (config->enable_tone_trackers) {
        snprintf(path, sizeof(path), "%s/wwv_carrier.csv", config->output_dir);
        mgr->tone_carrier = tone_tracker_create(0.0f, path);
        
        snprintf(path, sizeof(path), "%s/wwv_tone_500.csv", config->output_dir);
        mgr->tone_500 = tone_tracker_create(500.0f, path);
        
        snprintf(path, sizeof(path), "%s/wwv_tone_600.csv", config->output_dir);
        mgr->tone_600 = tone_tracker_create(600.0f, path);
    }
    
    if (config->enable_slow_marker) {
        mgr->slow_marker = slow_marker_detector_create();
        if (mgr->slow_marker) {
            slow_marker_detector_set_callback(mgr->slow_marker, wwv_routing_on_slow_marker_frame, mgr);
        }
    }
    
    printf("[DETECTOR_MGR] Created: tick=%s marker=%s sync=%s tones=%s slow=%s\n",
           mgr->tick_detector ? "YES" : "no",
           mgr->marker_detector ? "YES" : "no",
           mgr->sync_detector ? "YES" : "no",
           mgr->tone_carrier ? "YES" : "no",
           mgr->slow_marker ? "YES" : "no");
    
    return true;
}

/*============================================================================
 * Destruction
 *============================================================================*/

void wwv_detector_lifecycle_destroy_all(wwv_detector_manager_t *mgr) {
    if (!mgr) return;
    
    printf("[DETECTOR_MGR] Destroying...\n");
    
    /* Destroy in reverse order */
    if (mgr->slow_marker) slow_marker_detector_destroy(mgr->slow_marker);
    if (mgr->tone_600) tone_tracker_destroy(mgr->tone_600);
    if (mgr->tone_500) tone_tracker_destroy(mgr->tone_500);
    if (mgr->tone_carrier) tone_tracker_destroy(mgr->tone_carrier);
    if (mgr->sync_detector) sync_detector_destroy(mgr->sync_detector);
    if (mgr->marker_correlator) marker_correlator_destroy(mgr->marker_correlator);
    if (mgr->tick_correlator) tick_correlator_destroy(mgr->tick_correlator);
    if (mgr->marker_detector) marker_detector_destroy(mgr->marker_detector);
    if (mgr->tick_detector) tick_detector_destroy(mgr->tick_detector);
}
