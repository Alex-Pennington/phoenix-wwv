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
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
#include "wwv_time_decoder.h"
#include "channel_filters.h"
#include <stdint.h>

/*============================================================================
 * Internal State Structure
 *============================================================================*/

/* waterfall.c (1ec38d1) normalize(): slow AGC on |I/Q| */
typedef struct {
    float level;
    int warmup;
} wwv_normalizer_t;

struct wwv_detector_manager {
    bool verbose;
    bool agc;

    /* Detector path (50 kHz) */
    wwv_normalizer_t normalizer;
    sync_channel_t sync_i, sync_q;
    data_channel_t data_i, data_q;
    tick_detector_t *tick_detector;
    marker_detector_t *marker_detector;
    bcd_time_detector_t *bcd_time;
    bcd_freq_detector_t *bcd_freq;

    /* Correlators and decoder */
    tick_correlator_t *tick_correlator;
    marker_correlator_t *marker_correlator;
    sync_detector_t *sync_detector;
    bcd_correlator_t *bcd_correlator;
    wwv_time_decoder_t *time_decoder;

    /* Display path (12 kHz) */
    tone_tracker_t *tone_carrier;
    tone_tracker_t *tone_500;
    tone_tracker_t *tone_600;
    slow_marker_detector_t *slow_marker;

    /* External callbacks */
    wwv_detector_observer_t observer;
    wwv_tick_callback_fn tick_callback;
    void *tick_callback_data;
    wwv_marker_callback_fn marker_callback;
    void *marker_callback_data;
    wwv_sync_callback_fn sync_callback;
    void *sync_callback_data;

    /* Statistics */
    uint64_t detector_samples;
    uint64_t display_samples;
    wwv_time_ms_t now_ms;       /* time of the detector sample being processed */

    /* Filled gaps (wwv_detector_manager_mark_gap): a ring of spans, detector ms */
    struct { wwv_time_ms_t start_ms, end_ms; } gaps[WWV_GAP_SPANS];
    int n_gaps, gap_next;
    wwv_gap_drops_t gap_drops;
};

/** True when [lo_ms, hi_ms] overlaps a gap span widened by its guard and settle time */
bool wwv_gap_covers(const wwv_detector_manager_t *mgr, wwv_time_ms_t lo_ms, wwv_time_ms_t hi_ms);


/*============================================================================
 * Lifecycle Functions
 *============================================================================*/

/**
 * Create all detector components based on configuration, and wire them
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

/** Tick: observer, external callback */
void wwv_routing_on_tick_event(const tick_event_t *event, void *user_data);

/** Tick-path marker: sync detector (leading edge), tick epoch while it has none */
void wwv_routing_on_tick_marker_event(const tick_marker_event_t *event, void *user_data);

/** Marker detector: sync detector (leading edge), marker correlator, external callback */
void wwv_routing_on_marker_event(const marker_event_t *event, void *user_data);
void wwv_routing_on_tick_marker_candidate(const tick_marker_candidate_t *c, void *user_data);
void wwv_routing_on_marker_candidate(const marker_event_t *event, void *user_data);

/** Slow marker frames: marker correlator, for verification */
void wwv_routing_on_slow_marker_frame(const slow_marker_frame_t *frame, void *user_data);

/** BCD time / freq pulses: BCD correlator */
void wwv_routing_on_bcd_time_event(const bcd_time_event_t *event, void *user_data);
void wwv_routing_on_bcd_freq_event(const bcd_freq_event_t *event, void *user_data);

/** BCD symbol: time decoder */
void wwv_routing_on_bcd_symbol(const bcd_symbol_event_t *event, void *user_data);

/** Decoded minute */
void wwv_routing_on_time_result(const wwv_td_result_t *result, void *user_data);

/** Sync state change: observer, external sync callback */
void wwv_routing_on_sync_state(sync_state_t old_state, sync_state_t new_state,
                               float confidence, void *user_data);

#endif /* WWV_DETECTOR_MANAGER_INTERNAL_H */
