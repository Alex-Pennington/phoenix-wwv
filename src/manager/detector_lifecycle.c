/**
 * @file detector_lifecycle.c
 * @brief Detector component lifecycle management
 *
 * Creates, wires and destroys every detector component, based on configuration.
 */

#include "wwv_detector_manager_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*============================================================================
 * Creation
 *============================================================================*/

/* CSV path under output_dir, or NULL (no CSV) when output_dir is NULL */
static const char *csv_path(char *buf, size_t n, const wwv_detector_config_t *config,
                            const char *name) {
    if (!config->output_dir) return NULL;
    snprintf(buf, n, "%s/%s", config->output_dir, name);
    return buf;
}

bool wwv_detector_lifecycle_create_all(wwv_detector_manager_t *mgr,
                                        const wwv_detector_config_t *config) {
    char path[512];

    mgr->verbose = config->verbose;
    mgr->agc = config->enable_agc;
    mgr->normalizer.level = 0.01f;
    mgr->normalizer.warmup = 0;
    sync_channel_init(&mgr->sync_i);
    sync_channel_init(&mgr->sync_q);
    data_channel_init(&mgr->data_i);
    data_channel_init(&mgr->data_q);

    if (mgr->verbose) printf("\n[DETECTOR_MGR] Creating WWV detector manager...\n");

    /* Creation order is the order the air harness has always printed in. */
    if (config->enable_tick_detector) {
        mgr->tick_detector = tick_detector_create(csv_path(path, sizeof path, config, "wwv_ticks.csv"));
        if (!mgr->tick_detector) return false;
    }

    if (config->enable_marker_detector) {
        mgr->marker_detector = marker_detector_create(csv_path(path, sizeof path, config, "wwv_markers.csv"));
        if (!mgr->marker_detector) return false;
    }

    if (config->enable_sync_detector) {
        mgr->sync_detector = sync_detector_create(csv_path(path, sizeof path, config, "wwv_sync.csv"));
        if (!mgr->sync_detector) return false;
    }

    if (config->enable_bcd) {
        mgr->bcd_correlator = bcd_correlator_create(csv_path(path, sizeof path, config, "wwv_bcd_corr.csv"));
        if (!mgr->bcd_correlator) return false;
        if (config->enable_time_decoder) {
            mgr->time_decoder = wwv_time_decoder_create(NULL);
            if (!mgr->time_decoder) return false;
        }
        mgr->bcd_time = bcd_time_detector_create(csv_path(path, sizeof path, config, "wwv_bcd_time.csv"));
        mgr->bcd_freq = bcd_freq_detector_create(csv_path(path, sizeof path, config, "wwv_bcd_freq.csv"));
        if (!mgr->bcd_time || !mgr->bcd_freq) return false;
    }

    /* Correlators */
    if (config->enable_correlators) {
        mgr->tick_correlator = tick_correlator_create(csv_path(path, sizeof path, config, "wwv_tick_corr.csv"));
        mgr->marker_correlator = marker_correlator_create(csv_path(path, sizeof path, config, "wwv_markers_corr.csv"));
    }

    /* Display path components */
    if (config->enable_tone_trackers) {
        mgr->tone_carrier = tone_tracker_create(0.0f, csv_path(path, sizeof path, config, "wwv_carrier.csv"));
        mgr->tone_500 = tone_tracker_create(500.0f, csv_path(path, sizeof path, config, "wwv_tone_500.csv"));
        mgr->tone_600 = tone_tracker_create(600.0f, csv_path(path, sizeof path, config, "wwv_tone_600.csv"));
    }

    if (config->enable_slow_marker) {
        mgr->slow_marker = slow_marker_detector_create();
    }

    /* Wiring */
    if (mgr->tick_detector) {
        tick_detector_set_callback(mgr->tick_detector, wwv_routing_on_tick_event, mgr);
        tick_detector_set_marker_callback(mgr->tick_detector, wwv_routing_on_tick_marker_event, mgr);
        tick_detector_set_candidate_callback(mgr->tick_detector, wwv_routing_on_tick_marker_candidate, mgr);
    }
    if (mgr->marker_detector) {
        marker_detector_set_callback(mgr->marker_detector, wwv_routing_on_marker_event, mgr);
        marker_detector_set_candidate_callback(mgr->marker_detector, wwv_routing_on_marker_candidate, mgr);
    }
    if (mgr->bcd_time) bcd_time_detector_set_callback(mgr->bcd_time, wwv_routing_on_bcd_time_event, mgr);
    if (mgr->bcd_freq) bcd_freq_detector_set_callback(mgr->bcd_freq, wwv_routing_on_bcd_freq_event, mgr);
    if (mgr->bcd_correlator) {
        if (mgr->sync_detector) bcd_correlator_set_sync_source(mgr->bcd_correlator, mgr->sync_detector);
        bcd_correlator_set_callback(mgr->bcd_correlator, wwv_routing_on_bcd_symbol, mgr);
    }
    if (mgr->time_decoder) wwv_time_decoder_set_callback(mgr->time_decoder, wwv_routing_on_time_result, mgr);
    if (mgr->sync_detector) sync_detector_set_state_callback(mgr->sync_detector, wwv_routing_on_sync_state, mgr);
    if (mgr->slow_marker) {
        slow_marker_detector_set_callback(mgr->slow_marker, wwv_routing_on_slow_marker_frame, mgr);
    }

    if (mgr->verbose) {
        printf("[DETECTOR_MGR] Created: tick=%s marker=%s sync=%s bcd=%s decoder=%s agc=%s tones=%s slow=%s\n",
               mgr->tick_detector ? "YES" : "no",
               mgr->marker_detector ? "YES" : "no",
               mgr->sync_detector ? "YES" : "no",
               mgr->bcd_correlator ? "YES" : "no",
               mgr->time_decoder ? "YES" : "no",
               mgr->agc ? "YES" : "no",
               mgr->tone_carrier ? "YES" : "no",
               mgr->slow_marker ? "YES" : "no");
    }

    return true;
}

/*============================================================================
 * Destruction
 *============================================================================*/

void wwv_detector_lifecycle_destroy_all(wwv_detector_manager_t *mgr) {
    if (!mgr) return;

    if (mgr->verbose) printf("[DETECTOR_MGR] Destroying...\n");

    /* The BCD correlator first: destroying it closes its open window, and the
     * decoder it reports to must still exist. */
    if (mgr->bcd_correlator) bcd_correlator_destroy(mgr->bcd_correlator);
    if (mgr->time_decoder) wwv_time_decoder_destroy(mgr->time_decoder);
    if (mgr->bcd_freq) bcd_freq_detector_destroy(mgr->bcd_freq);
    if (mgr->bcd_time) bcd_time_detector_destroy(mgr->bcd_time);

    /* Then the rest in reverse order */
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
