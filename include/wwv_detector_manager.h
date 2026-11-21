/**
 * @file wwv_detector_manager.h
 * @brief Centralized WWV detector orchestration: the one wiring of the detector chain
 *
 * The manager owns every detector, the sync detector, the BCD correlator and the
 * time decoder, and wires them. A host feeds it baseband I/Q with the carrier at DC
 * and reads the results from callbacks; it does no routing of its own.
 *
 * DETECTOR PATH, 50 kHz (wwv_detector_manager_process_detector_sample), per sample:
 *
 *   I/Q ── [slow AGC, optional] ──┬── sync channel 800-1400 Hz ──┬── tick_detector
 *                                 │                             └── marker_detector
 *                                 └── data channel 50-150 Hz ───┬── bcd_time_detector
 *                                                               └── bcd_freq_detector
 *
 *   tick marker      -> sync_detector_tick_marker(leading edge), and the tick
 *                       detector's epoch while it has none
 *   marker detector  -> sync_detector_marker_event(leading edge)
 *   BCD time / freq  -> bcd_correlator (gated on the sync detector)
 *   BCD symbol       -> wwv_time_decoder
 *   every 2500 samples (50 ms), after the sample: sync_detector_periodic_check,
 *                       bcd_correlator_periodic_check, wwv_time_decoder_periodic
 *
 * Timestamps are the detectors' own: ms since detector sample 0. A host that knows
 * the UTC of sample 0 maps them; the manager never reads a clock.
 *
 * DISPLAY PATH, 12 kHz (wwv_detector_manager_process_display_sample): tone
 * trackers (carrier, 500 Hz, 600 Hz). slow_marker_detector takes display FFT frames
 * (wwv_detector_manager_process_display_fft) and feeds only the marker correlator.
 *
 * DESIGN RULES:
 *   1. Detectors are self-contained: no cross-path baseline sharing.
 *   2. Energy values from different FFT paths are not comparable.
 *   3. Detector callbacks are handled here, then forwarded to the observer.
 */

#ifndef WWV_DETECTOR_MANAGER_H
#define WWV_DETECTOR_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include "external/kiss_fft.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
#include "sync_detector.h"
#include "wwv_time_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Opaque handle
 *============================================================================*/

typedef struct wwv_detector_manager wwv_detector_manager_t;

/*============================================================================
 * Configuration
 *============================================================================*/

#define WWV_DETECTOR_SAMPLE_RATE     50000   /* detector path, Hz */
#define WWV_DISPLAY_SAMPLE_RATE      12000   /* display / tone path, Hz */
#define WWV_PERIODIC_CHECK_SAMPLES   2500    /* 50 ms at 50 kHz */

typedef struct {
    const char *output_dir;         /* Directory for CSV logs; NULL writes none */
    bool enable_tick_detector;
    bool enable_marker_detector;
    bool enable_sync_detector;
    bool enable_tone_trackers;
    bool enable_correlators;        /* tick and marker correlators (diagnostic) */
    bool enable_slow_marker;        /* Display-path marker verification */
    bool enable_bcd;                /* BCD time/freq detectors and the BCD correlator */
    bool enable_time_decoder;       /* UTC from the BCD symbols (needs enable_bcd) */
    bool enable_agc;                /* waterfall.c slow AGC before the channel filters */
    bool verbose;                   /* manager's own create/destroy/stats lines */
} wwv_detector_config_t;

/* Default config - the full chain, no AGC */
#define WWV_DETECTOR_CONFIG_DEFAULT { \
    .output_dir = ".", \
    .enable_tick_detector = true, \
    .enable_marker_detector = true, \
    .enable_sync_detector = true, \
    .enable_tone_trackers = true, \
    .enable_correlators = true, \
    .enable_slow_marker = true, \
    .enable_bcd = true, \
    .enable_time_decoder = true, \
    .enable_agc = false, \
    .verbose = true \
}

/*============================================================================
 * Callbacks for external notification
 *============================================================================*/

/* Tick detected */
typedef struct {
    int tick_number;
    float timestamp_ms;
    float duration_ms;
    float energy;
} wwv_tick_event_t;

typedef void (*wwv_tick_callback_fn)(const wwv_tick_event_t *event, void *user_data);

/* Minute marker detected */
typedef struct {
    int marker_number;
    float timestamp_ms;
    float since_last_sec;
    float duration_ms;
    float energy;
} wwv_marker_event_t;

typedef void (*wwv_marker_callback_fn)(const wwv_marker_event_t *event, void *user_data);

/* Sync status update */
typedef struct {
    bool is_synced;
    int confidence;             /* 0-100 */
    float drift_ppm;
    int tick_count;
    int marker_count;
} wwv_sync_status_t;

typedef void (*wwv_sync_callback_fn)(const wwv_sync_status_t *status, void *user_data);

/**
 * Observer: every event of the chain, raw. Any member may be NULL.
 *
 * Each is called before the manager routes the event, except the *_routed members,
 * which are called after the BCD correlator has taken the event. The observer must
 * not feed events into the chain itself.
 */
typedef struct {
    void (*on_tick)(const tick_event_t *e, void *user);
    void (*on_tick_marker)(const tick_marker_event_t *e, void *user);
    void (*on_marker)(const marker_event_t *e, void *user);
    void (*on_tick_marker_candidate)(const tick_marker_candidate_t *c, void *user);
    void (*on_marker_candidate)(const marker_event_t *e, void *user);
    void (*on_bcd_time)(const bcd_time_event_t *e, void *user);
    void (*on_bcd_time_routed)(const bcd_time_event_t *e, void *user);
    void (*on_bcd_freq)(const bcd_freq_event_t *e, void *user);
    void (*on_bcd_freq_routed)(const bcd_freq_event_t *e, void *user);
    void (*on_symbol)(const bcd_symbol_event_t *e, void *user);     /* before the decoder */
    void (*on_time)(const wwv_td_result_t *r, void *user);
    void (*on_sync_state)(sync_state_t old_state, sync_state_t new_state,
                          float confidence, void *user);
    void *user;
} wwv_detector_observer_t;

/*============================================================================
 * Lifecycle
 *============================================================================*/

/**
 * Create detector manager with given configuration
 */
wwv_detector_manager_t *wwv_detector_manager_create(const wwv_detector_config_t *config);

/**
 * Destroy detector manager and all owned detectors. The BCD correlator is
 * destroyed first, while the decoder and observer can still see its last window.
 */
void wwv_detector_manager_destroy(wwv_detector_manager_t *mgr);

/*============================================================================
 * Sample Processing
 *============================================================================*/

/**
 * Process one detector-path I/Q sample: 50 kHz baseband, carrier at DC, before any
 * channel filter (scaled as int16/32768 for the tuned thresholds). Runs AGC (if
 * enabled), channel filters, all detectors, then the periodic checks.
 */
void wwv_detector_manager_process_detector_sample(wwv_detector_manager_t *mgr,
                                                   float i_sample, float q_sample);

/**
 * Process display-path I/Q sample (12 kHz)
 * Feeds: tone_trackers
 */
void wwv_detector_manager_process_display_sample(wwv_detector_manager_t *mgr,
                                                  float i_sample, float q_sample);

/**
 * Process display-path FFT output (for slow marker detector)
 */
void wwv_detector_manager_process_display_fft(wwv_detector_manager_t *mgr,
                                               const kiss_fft_cpx *fft_out,
                                               float timestamp_ms);

/*============================================================================
 * Callbacks
 *============================================================================*/

void wwv_detector_manager_set_observer(wwv_detector_manager_t *mgr,
                                        const wwv_detector_observer_t *observer);

void wwv_detector_manager_set_tick_callback(wwv_detector_manager_t *mgr,
                                             wwv_tick_callback_fn cb, void *user_data);

void wwv_detector_manager_set_marker_callback(wwv_detector_manager_t *mgr,
                                               wwv_marker_callback_fn cb, void *user_data);

void wwv_detector_manager_set_sync_callback(wwv_detector_manager_t *mgr,
                                             wwv_sync_callback_fn cb, void *user_data);

/*============================================================================
 * Status / Diagnostics
 *============================================================================*/

/**
 * Get current sync status
 */
wwv_sync_status_t wwv_detector_manager_get_sync_status(wwv_detector_manager_t *mgr);

/**
 * Detector samples processed, and the time (ms since sample 0) of the sample being
 * processed (the last one, between calls).
 */
uint64_t wwv_detector_manager_get_detector_samples(wwv_detector_manager_t *mgr);
double wwv_detector_manager_get_now_ms(wwv_detector_manager_t *mgr);

/**
 * The owned components, for diagnostics (NULL when disabled). Do not destroy them,
 * and do not replace their callbacks.
 */
tick_detector_t *wwv_detector_manager_get_tick_detector(wwv_detector_manager_t *mgr);
marker_detector_t *wwv_detector_manager_get_marker_detector(wwv_detector_manager_t *mgr);
sync_detector_t *wwv_detector_manager_get_sync_detector(wwv_detector_manager_t *mgr);
bcd_correlator_t *wwv_detector_manager_get_bcd_correlator(wwv_detector_manager_t *mgr);
wwv_time_decoder_t *wwv_detector_manager_get_time_decoder(wwv_detector_manager_t *mgr);

/**
 * Get detector counts for display
 */
int wwv_detector_manager_get_tick_count(wwv_detector_manager_t *mgr);
int wwv_detector_manager_get_marker_count(wwv_detector_manager_t *mgr);

/**
 * Get flash frames for UI (tick and marker combined)
 */
int wwv_detector_manager_get_tick_flash(wwv_detector_manager_t *mgr);
int wwv_detector_manager_get_marker_flash(wwv_detector_manager_t *mgr);
void wwv_detector_manager_decrement_flash(wwv_detector_manager_t *mgr);

/**
 * Log metadata change (frequency, gain, etc.)
 */
void wwv_detector_manager_log_metadata(wwv_detector_manager_t *mgr,
                                        uint64_t center_freq,
                                        uint32_t sample_rate,
                                        uint32_t gain_reduction,
                                        uint32_t lna_state);

void wwv_detector_manager_log_display_gain(wwv_detector_manager_t *mgr, float display_gain);

/**
 * Samples lost upstream and filled (zeros) by the host: [start_ms, start_ms + len_ms)
 * in detector time. Call before the filled span is processed. Until the span's
 * settle time has passed, routing still reports each detector event to the observer
 * but does not pass it to sync, the correlators, the decoder or the external
 * callbacks when its extent overlaps [start_ms - WWV_GAP_GUARD_MS,
 * start_ms + len_ms + settle], settle = min(WWV_GAP_SETTLE_MS, WWV_GAP_SETTLE_MIN_MS
 * + len_ms): an event the zeros made or cut is not evidence. (On the synthetic
 * chain the detectors' onset events at a span's end come within 40 ms of it.) A BCD
 * window with no event is an erasure. Abutting or overlapping spans merge. Up to
 * WWV_GAP_SPANS spans are kept; the oldest is dropped first.
 */
#define WWV_GAP_GUARD_MS      20.0
#define WWV_GAP_SETTLE_MIN_MS 100.0
#define WWV_GAP_SETTLE_MS     1500.0
#define WWV_GAP_SPANS      8
void wwv_detector_manager_mark_gap(wwv_detector_manager_t *mgr, double start_ms, double len_ms);

/** Events held back from routing by gaps, cumulative since create */
typedef struct { int tick, tmark, marker, candidate, btime, bfreq; } wwv_gap_drops_t;
wwv_gap_drops_t wwv_detector_manager_get_gap_drops(wwv_detector_manager_t *mgr);

/**
 * Print statistics for all detectors
 */
void wwv_detector_manager_print_stats(wwv_detector_manager_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* WWV_DETECTOR_MANAGER_H */
