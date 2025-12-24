/**
 * @file tick_internal.h
 * @brief Internal interfaces for tick detector modules
 *
 * Private header shared between tick_detector.c, tick_correlation.c,
 * and tick_state_machine.c. NOT part of public API.
 */

#ifndef TICK_INTERNAL_H
#define TICK_INTERNAL_H

#include "tick_detector.h"
#include "wwv_clock.h"
#include "tick_comb_filter.h"
#include "fft_processor.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Internal Configuration Constants
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)TICK_FFT_SIZE * 1000.0f / TICK_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)TICK_SAMPLE_RATE / TICK_FFT_SIZE)

/* Detection timing */
#define TICK_MIN_DURATION_MS    2.0f
#define TICK_MAX_DURATION_MS    50.0f
#define MARKER_MAX_DURATION_MS  1000.0f  /* Allow up to 1s for minute markers */
#define TICK_COOLDOWN_MS        500.0f

/* Threshold adaptation */
#define TICK_NOISE_ADAPT_DOWN   0.002f   /* Fast attack when signal drops */
#define TICK_NOISE_ADAPT_UP     0.0002f  /* Slow decay to prevent learning ticks */
#define NOISE_FLOOR_MAX         5.0f
#define TICK_WARMUP_ADAPT_RATE  0.05f
#define TICK_HYSTERESIS_RATIO   0.7f
#define TICK_THRESHOLD_MULT     2.0f

/* Correlation thresholds */
#define CORR_THRESHOLD_MULT     5.0f    /* Correlation must be 5x noise floor */
#define CORR_NOISE_ADAPT        0.01f   /* Noise floor adaptation rate */
#define CORR_DECIMATION         8       /* Compute correlation every N samples */
#define MARKER_CORR_RATIO       15.0f   /* Corr ratio above this = minute marker */
#define MARKER_MIN_DURATION_MS  600.0f  /* Marker must be at least 600ms (tightened from 500ms) */
#define MARKER_MAX_DURATION_MS_CHECK 1500.0f  /* Marker should be under 1500ms */
#define MARKER_MIN_INTERVAL_MS  55000.0f /* Markers must be 55+ seconds apart */

/* Warmup and display */
#define TICK_WARMUP_FRAMES      50
#define TICK_FLASH_FRAMES       5

/* History for averaging */
#define TICK_HISTORY_SIZE       30
#define TICK_AVG_WINDOW_MS      15000.0f

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

/* Timing gate for exploiting NIST 40ms protected zone */
#define TICK_GATE_START_MS   0.0f    /* Open gate at second boundary */
#define TICK_GATE_END_MS   100.0f   /* Close gate 100ms into second (was 25ms - too narrow for HF) */

/* Gate recovery - disable gate if no ticks for too long */
#define GATE_RECOVERY_MS   5000.0f  /* 5 seconds without tick = disable gate temporarily */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State Types
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_TICK,
    STATE_COOLDOWN
} detector_state_t;

typedef struct {
    float epoch_ms;          /* Second boundary offset (from marker) */
    bool enabled;            /* Gate is active */
    uint64_t last_tick_frame_gated; /* Frame when last tick detected with gate enabled */
    bool recovery_mode;      /* True when gate temporarily disabled for recovery */
} tick_gate_t;

/*============================================================================
 * Detector State Structure
 *============================================================================*/

struct tick_detector {
    /* FFT resources */
    fft_processor_t *fft;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Matched filter resources */
    float *template_i;          /* Cosine template */
    float *template_q;          /* Sine template */
    float *corr_buf_i;          /* Circular buffer for correlation */
    float *corr_buf_q;
    int corr_buf_idx;           /* Write position in circular buffer */
    int corr_sample_count;      /* Total samples received */
    float corr_peak;            /* Peak correlation value this detection */
    float corr_sum;             /* Accumulated correlation during pulse */
    int corr_sum_count;         /* Number of correlation samples accumulated */
    int corr_peak_offset;       /* Sample offset of peak */
    float corr_noise_floor;     /* Correlation noise floor estimate */

    /* Detection state */
    detector_state_t state;
    float noise_floor;
    float threshold_high;
    float threshold_low;
    float current_energy;

    /* Tick measurement */
    uint64_t tick_start_frame;
    float tick_peak_energy;
    int tick_duration_frames;
    int cooldown_frames;

    /* Statistics */
    int ticks_detected;
    int ticks_rejected;
    int markers_detected;       /* Position/minute markers (long pulses) */
    uint64_t last_tick_frame;
    uint64_t last_marker_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* History for interval averaging */
    float tick_timestamps_ms[TICK_HISTORY_SIZE];
    int tick_history_idx;
    int tick_history_count;

    /* UI feedback */
    int flash_frames_remaining;
    bool detection_enabled;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float threshold_multiplier;     /* Detection sensitivity (1.0-5.0, default 2.0) */
    float adapt_alpha_down;         /* Noise floor decay rate (0.9-0.999, default 0.995) */
    float adapt_alpha_up;           /* Noise floor rise rate (0.001-0.1, default 0.02) */
    float min_duration_ms;          /* Minimum pulse width (1.0-10.0, default 2.0) */

    /* Callback */
    tick_callback_fn callback;
    void *callback_user_data;

    /* Marker callback */
    tick_marker_callback_fn marker_callback;
    void *marker_callback_user_data;

    /* Logging */
    FILE *csv_file;
    time_t start_time;          /* Wall clock time when detector started */

    /* WWV broadcast clock */
    wwv_clock_t *wwv_clock;

    /* Timing gate */
    tick_gate_t gate;
    epoch_source_t epoch_source;
    float epoch_confidence;

    /* Comb filter for weak signal detection */
    comb_filter_t *comb_filter;
};

/*============================================================================
 * Internal Function Declarations
 *============================================================================*/

/* From tick_correlation.c */
void tick_correlation_init(tick_detector_t *td);
float tick_correlation_compute(tick_detector_t *td);

/* From tick_state_machine.c */
void tick_state_machine_run(tick_detector_t *td);
bool tick_state_is_gate_open(tick_detector_t *td, float current_ms);

/* Helper functions (remain in tick_detector.c) */
float tick_calculate_avg_interval(tick_detector_t *td, float current_time_ms);
void tick_get_wall_time_str(tick_detector_t *td, float timestamp_ms, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* TICK_INTERNAL_H */
