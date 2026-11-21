/**
 * @file bcd_time_detector.h
 * @brief WWV BCD time-domain detector module
 *
 * Time-domain focused FFT for precise pulse edge detection at 100Hz.
 * Uses small FFT (256) for fast time response at expense of frequency resolution.
 *
 * Pattern: Follows tick_detector.h structure
 *
 * Design notes:
 *   - Self-contained detector with own FFT (50kHz, 256-pt)
 *   - Own sample buffer and state machine
 *   - Designed to run in parallel with bcd_freq_detector
 *   - Reports events with precise edge timestamps
 *   - Works with bcd_correlator for dual-path symbol confirmation
 */

#ifndef BCD_TIME_DETECTOR_H
#define BCD_TIME_DETECTOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "wwv_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration - Tune these for 100Hz BCD signal
 *============================================================================*/

#define BCD_TIME_FFT_SIZE           256     /* 5.12ms frames at 50kHz */
#define BCD_TIME_SAMPLE_RATE        50000   /* Input sample rate (2MHz/40) */
#define BCD_TIME_TARGET_FREQ_HZ     100     /* BCD subcarrier frequency */
#define BCD_TIME_BANDWIDTH_HZ       50      /* Wider bucket for coarse resolution */
/* At 195 Hz/bin this bucket is bins 0-2 (0-390 Hz, DC included): a 5.12 ms frame
 * cannot resolve 100 Hz, so this detector measures in-band energy only. Its 100 Hz
 * selectivity and carrier rejection come from the data channel's 50-150 Hz
 * bandpass (channel_filters.c), which nulls DC. Feed it data_channel_process()
 * output, never raw baseband. */

/* Detection parameters */
#define BCD_TIME_THRESHOLD_MULT     2.0f    /* Energy must be 2x noise floor */
#define BCD_TIME_HYSTERESIS_RATIO   0.7f    /* Drop-off threshold */
/* A pulse ends where its energy falls below this fraction of its own mean level:
 * width at half amplitude (the bucket value is a magnitude). WWV keeps the 100 Hz
 * subcarrier on at a low level (15 dB under the pulse) between pulses, and the floor
 * settles under that low level, so threshold_low alone does not see a pulse end. */
#define BCD_TIME_EDGE_FRACTION      0.5f
/* A pulse also starts at half amplitude: the start threshold is BCD_TIME_EDGE_FRACTION
 * of the running mean level of recent valid pulses (each pulse's mean, averaged with
 * weight BCD_TIME_LEVEL_ALPHA), or threshold_high or the low-level term below (see
 * BCD_TIME_START_LOW_RATIO), whichever is highest. threshold_high alone sits
 * on the low-level subcarrier and starts events between pulses, whose cooldown hides
 * the next rise. WWV sends a pulse every second except :00, so after
 * BCD_TIME_LEVEL_HOLD_MS with no valid pulse the level is stale (a fade) and is
 * dropped until the next valid pulse. */
#define BCD_TIME_LEVEL_ALPHA        0.25f
#define BCD_TIME_LEVEL_HOLD_MS      2000.0f
/* A pulse is a step up from the low level, so a start must also clear
 * BCD_TIME_START_LOW_RATIO times the low level: 7.5 dB, half of WWV's 15 dB
 * pulse/low step in dB. The low level is the median energy over the second half
 * (100-200 ms after the event's end) of the last event's cooldown, which follows the
 * end of a pulse. An event that ends early, on a dip of the 100 Hz phase ripple, leaves
 * the rest of its pulse at the start of the cooldown, where neither the half nor the
 * median sees it. It is not taken from idle frames, which may hold
 * a pulse too weak to start, and it is dropped with the pulse level. Without it the
 * start threshold falls to 1.3-1.5 times the low level when the pulse level is low or
 * stale, and a start on the low level runs through the next second's pulse (its end
 * is half its own mean). */
#ifndef BCD_TIME_START_LOW_RATIO
#define BCD_TIME_START_LOW_RATIO    2.37f
#endif

/* Width from a step fit. At low pulse/low contrast the low level sits above half the
 * event's mean, so the half-amplitude end comes on a dip of the 100 Hz phase ripple or
 * of the noise, and a 1 or a P reads short. So an event whose edge width is at most
 * BCD_TIME_FIT_EDGE_MAX_MS is held until BCD_TIME_FIT_SPAN_MS after its start, and a
 * box is fitted to the log energy from BCD_TIME_FIT_PRE_MS before the start to
 * BCD_TIME_FIT_SPAN_MS after it: one level inside [rise, end), one outside, least
 * squares, the rise at most BCD_TIME_FIT_RISE_MAX_MS after the start, the box at least
 * BCD_TIME_FIT_MIN_WIDTH_MS wide and ending at least BCD_TIME_FIT_TAIL_FRAMES frames
 * before the window does. The log keeps a noise burst in the low part from moving the
 * fit. The event's width is the box's width plus BCD_TIME_FIT_OFFSET_MS (the edge
 * width's own excess over the sent high part) when the fit's contrast, exp of the
 * inside less the outside mean log energy, is at least BCD_TIME_FIT_CONTRAST_MIN and
 * under BCD_TIME_FIT_CONTRAST_MAX. Under the minimum the fit is noise; at the maximum
 * or above the half-amplitude edges are reliable, and a low level that steps inside
 * the window (a fade) would pull a two-level fit. Otherwise the edge width stands.
 * The event's start time is unchanged; it is reported when its window ends, and
 * events are reported in start order. */
#define BCD_TIME_FIT_EDGE_MAX_MS    850.0f
#define BCD_TIME_FIT_PRE_MS         200.0f
#define BCD_TIME_FIT_SPAN_MS        900.0f
#define BCD_TIME_FIT_RISE_MAX_MS    30.0f
#define BCD_TIME_FIT_MIN_WIDTH_MS   80.0f
#define BCD_TIME_FIT_TAIL_FRAMES    6
#define BCD_TIME_FIT_OFFSET_MS      15.0f
#define BCD_TIME_FIT_CONTRAST_MIN   1.3f
#define BCD_TIME_FIT_CONTRAST_MAX   3.0f

/* Pulse classification thresholds (same as existing bcd_decoder) */
#define BCD_TIME_PULSE_MIN_MS       100.0f  /* Ignore pulses shorter than this */
/* An event that starts on the low level up to 250 ms before a second and runs
 * through that second's P (high to about +850 ms) is up to 1100 ms long. At 1000
 * it was rejected and the P left no event and no trailing edge. */
#define BCD_TIME_PULSE_MAX_MS       1100.0f /* Ignore pulses longer than this */

/*============================================================================
 * Detector State (opaque to caller)
 *============================================================================*/

typedef struct bcd_time_detector bcd_time_detector_t;

/*============================================================================
 * Callback for pulse events
 *============================================================================*/

typedef struct {
    wwv_time_ms_t timestamp_ms; /* When pulse started */
    float duration_ms;          /* Pulse width */
    float peak_energy;          /* Peak energy during pulse */
    float noise_floor;          /* Noise floor at detection */
    float snr_db;               /* Signal-to-noise ratio */
} bcd_time_event_t;

typedef void (*bcd_time_callback_fn)(const bcd_time_event_t *event, void *user_data);

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * Create a new BCD time-domain detector instance
 * @param csv_path  Path for CSV log file (NULL to disable logging)
 * @return          Detector instance or NULL on failure
 */
bcd_time_detector_t *bcd_time_detector_create(const char *csv_path);

/**
 * Destroy a BCD time-domain detector instance
 */
void bcd_time_detector_destroy(bcd_time_detector_t *td);

/**
 * Set callback for pulse events
 * @param td        Detector instance
 * @param callback  Function to call when pulse detected
 * @param user_data Passed to callback
 */
void bcd_time_detector_set_callback(bcd_time_detector_t *td,
                                    bcd_time_callback_fn callback,
                                    void *user_data);

/**
 * Feed I/Q samples to detector
 * Detector buffers internally and runs FFT when ready
 * @param td        Detector instance
 * @param i_sample  In-phase sample
 * @param q_sample  Quadrature sample
 * @return          true if a pulse was detected this sample
 */
bool bcd_time_detector_process_sample(bcd_time_detector_t *td,
                                      float i_sample,
                                      float q_sample);

/**
 * Enable/disable detection
 */
void bcd_time_detector_set_enabled(bcd_time_detector_t *td, bool enabled);
bool bcd_time_detector_get_enabled(bcd_time_detector_t *td);

/**
 * Get current state for display/debug
 */
float bcd_time_detector_get_noise_floor(bcd_time_detector_t *td);
float bcd_time_detector_get_threshold(bcd_time_detector_t *td);
float bcd_time_detector_get_current_energy(bcd_time_detector_t *td);
int bcd_time_detector_get_pulse_count(bcd_time_detector_t *td);

/**
 * Print statistics to stdout
 */
void bcd_time_detector_print_stats(bcd_time_detector_t *td);

/**
 * Get timing info
 */
float bcd_time_detector_get_frame_duration_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* BCD_TIME_DETECTOR_H */
