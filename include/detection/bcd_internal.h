/**
 * @file bcd_internal.h
 * @brief Internal structures and state machine declarations for BCD detectors
 *
 * Shared internal header for:
 *   - bcd_time_detector (time-domain, precise edge timing)
 *   - bcd_freq_detector (frequency-domain, confident presence detection)
 *
 * Both detectors share the same 3-state FSM pattern and similar structure.
 * This header exposes internals to allow state machine extraction.
 *
 * Pattern: Follows tick_internal.h and marker_internal.h
 */

#ifndef BCD_INTERNAL_H
#define BCD_INTERNAL_H

#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "fft_processor.h"
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Common Configuration
 *============================================================================*/

#define NOISE_FLOOR_MIN         0.0001f
#define NOISE_FLOOR_MAX         5.0f
#define MIN_LOW_FRAMES          3       /* Debounce pulse end */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Common State Machine States
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_PULSE,
    STATE_COOLDOWN
} detector_state_t;

/*============================================================================
 * BCD Time Detector Internal Structure
 *============================================================================*/

struct bcd_time_detector {
    /* FFT resources */
    fft_processor_t *fft;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Detection state */
    detector_state_t state;
    float noise_floor;
    float threshold_high;
    float threshold_low;
    float current_energy;

    /* Pulse measurement */
    uint64_t pulse_start_frame;
    float pulse_peak_energy;
    int pulse_duration_frames;
    int cooldown_frames;

    /* Phase 9: Minimum duration validation */
    int consecutive_low_frames;

    /* Statistics */
    int pulses_detected;
    int pulses_rejected;
    uint64_t last_pulse_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* Enabled flag */
    bool detection_enabled;

    /* Callback */
    bcd_time_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * BCD Frequency Detector Internal Structure
 *============================================================================*/

struct bcd_freq_detector {
    /* FFT resources */
    fft_processor_t *fft;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Sliding window accumulator */
    float *energy_history;
    int history_idx;
    int history_count;
    float accumulated_energy;
    float baseline_energy;

    /* Detection state */
    detector_state_t state;
    float current_energy;
    float threshold;

    /* Pulse measurement */
    uint64_t pulse_start_frame;
    float pulse_peak_energy;
    int pulse_duration_frames;
    int cooldown_frames;

    /* Phase 9: Minimum duration validation */
    int consecutive_low_frames;

    /* Statistics */
    int pulses_detected;
    int pulses_rejected;
    uint64_t last_pulse_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* Enabled flag */
    bool detection_enabled;

    /* Callback */
    bcd_freq_callback_fn callback;
    void *callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;
};

/*============================================================================
 * Common Helper Functions
 *============================================================================*/

/**
 * Get wall clock time string for CSV output
 */
static inline void bcd_get_wall_time_str(time_t start_time, float timestamp_ms, 
                                         char *buf, size_t buflen) {
    time_t event_time = start_time + (time_t)(timestamp_ms / 1000.0f);
    struct tm *tm_info = localtime(&event_time);
    strftime(buf, buflen, "%H:%M:%S", tm_info);
}

/*============================================================================
 * BCD Time Detector State Machine Functions
 *============================================================================*/

/**
 * Calculate energy in the 100Hz frequency bucket for time detector
 */
float bcd_time_calculate_bucket_energy(bcd_time_detector_t *td);

/**
 * Run the time detector state machine
 * Called once per FFT frame after energy calculation
 */
void bcd_time_run_state_machine(bcd_time_detector_t *td);

/*============================================================================
 * BCD Frequency Detector State Machine Functions
 *============================================================================*/

/**
 * Calculate energy in the 100Hz frequency bucket for freq detector
 */
float bcd_freq_calculate_bucket_energy(bcd_freq_detector_t *fd);

/**
 * Update sliding window accumulator with new energy value
 */
void bcd_freq_update_accumulator(bcd_freq_detector_t *fd, float energy);

/**
 * Run the frequency detector state machine
 * Called once per FFT frame after energy calculation
 */
void bcd_freq_run_state_machine(bcd_freq_detector_t *fd);

#ifdef __cplusplus
}
#endif

#endif /* BCD_INTERNAL_H */
