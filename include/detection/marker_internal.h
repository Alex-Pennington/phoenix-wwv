/**
 * @file marker_internal.h
 * @brief Internal interfaces for marker detector modules
 *
 * Private header shared between marker_detector.c and marker_state_machine.c.
 * NOT part of public API.
 */

#ifndef MARKER_INTERNAL_H
#define MARKER_INTERNAL_H

#include "marker_detector.h"
#include "wwv_clock.h"
#include "fft_processor.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Internal Configuration Constants
 *============================================================================*/

#define FRAME_DURATION_MS   ((float)MARKER_FFT_SIZE * 1000.0f / MARKER_SAMPLE_RATE)
#define HZ_PER_BIN          ((float)MARKER_SAMPLE_RATE / MARKER_FFT_SIZE)

/* Detection thresholds - proven values from v133 */
#define MARKER_THRESHOLD_MULT       3.0f    /* Accumulated must be 3x baseline */
#define MARKER_NOISE_ADAPT_RATE     0.001f  /* Slow baseline adaptation */
#define MARKER_COOLDOWN_MS          30000.0f /* 30 sec between markers (they're 60 sec apart) */
#define MARKER_MAX_DURATION_MS      5000.0f /* Max time in IN_MARKER before forced exit */

/* Warmup */
#define MARKER_WARMUP_FRAMES        200     /* ~1 second warmup */
#define MARKER_WARMUP_ADAPT_RATE    0.02f   /* Faster adaptation during warmup */
#define MARKER_MIN_STARTUP_MS       10000.0f /* No markers in first 10 seconds */

/* Tone purity. A WWV minute marker is a pure 1000 Hz tone. Through the 256-point
 * Hann window its energy sits in the centre bin (5, 977 Hz) and leaks about 0.04
 * of that into the bins two either side (3 and 7, 586 and 1367 Hz). Broadband
 * audio (the :52.5 voice announcement, noise) puts as much in those flank bins as
 * in the centre (ratio ~1.0 through the sync filter). The test compares the
 * EXCESS over the pre-event background, so it does not depend on SNR. The
 * threshold is the geometric mean of 0.04 and 1.0. */
#define MARKER_FLANK_OFFSET_BINS    2
#define MARKER_MAX_FLANK_RATIO      0.2f
#define MARKER_PURITY_RING          1024    /* frames (5.2 s): covers the longest run */

/* Logging: one "MARKER block peak" line per block */
#define MARKER_PEAK_LOG_MS          10000.0f

/* Display */
#define MARKER_FLASH_FRAMES         30      /* UI flash duration */

#define MS_TO_FRAMES(ms)    ((int)((ms) / FRAME_DURATION_MS + 0.5f))

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/*============================================================================
 * Internal State Types
 *============================================================================*/

typedef enum {
    STATE_IDLE,
    STATE_IN_MARKER,
    STATE_COOLDOWN
} detector_state_t;

/*============================================================================
 * Detector State Structure
 *============================================================================*/

struct marker_detector {
    /* FFT resources */
    fft_processor_t *fft;

    /* Sample buffer for FFT */
    float *i_buffer;
    float *q_buffer;
    int buffer_idx;

    /* Sliding window accumulator */
    float *energy_history;          /* Circular buffer of frame energies */
    int history_idx;                /* Write position */
    int history_count;              /* Frames accumulated so far */
    float accumulated_energy;       /* Sum of energy_history */
    float baseline_energy;          /* Self-tracked noise floor */

    /* Tone purity: per-frame centre-bin and flank-bin levels, and their
     * backgrounds (tracked like baseline_energy, frozen in a run) */
    float *mag_buffer;              /* FFT magnitudes, MARKER_FFT_SIZE */
    float *centre_ring;
    float *flank_ring;
    float bg_centre;
    float bg_flank;
    float last_flank_ratio;         /* Of the last completed run, for logging */

    /* Detection state */
    detector_state_t state;
    float current_energy;           /* Current frame's 1000Hz bucket energy */
    float threshold;                /* Detection threshold */

    /* Marker measurement */
    uint64_t marker_start_frame;
    float marker_peak_energy;
    int marker_duration_frames;
    int cooldown_frames;

    /* Logging: the largest window sum over baseline in each MARKER_PEAK_LOG_MS
     * block, so a minute with no run shows how far its marker fell short */
    float block_peak_ratio;
    uint64_t block_peak_frame;
    uint64_t block_start_frame;

    /* Statistics */
    int markers_detected;
    uint64_t last_marker_frame;
    uint64_t frame_count;
    uint64_t start_frame;
    bool warmup_complete;

    /* UI feedback */
    int flash_frames_remaining;
    bool detection_enabled;

    /* Tunable parameters (runtime adjustable via UDP commands) */
    float threshold_multiplier;     /* Threshold above baseline (2.0-5.0, default 3.0) */
    float noise_adapt_rate;         /* Baseline adaptation rate (0.0001-0.01, default 0.001) */
    float min_duration_ms;          /* Minimum pulse duration (300.0-700.0, default 500.0) */

    /* Callback */
    marker_callback_fn callback;
    void *callback_user_data;
    marker_callback_fn candidate_callback;
    void *candidate_callback_user_data;

    /* Logging */
    FILE *csv_file;
    FILE *debug_file;
    time_t start_time;

    /* WWV clock for expected event lookup */
    wwv_clock_t *wwv_clock;
};

/*============================================================================
 * Internal Function Declarations
 *============================================================================*/

/* From marker_state_machine.c */
void marker_state_machine_run(marker_detector_t *md);

/* Helper functions (remain in marker_detector.c) */
void marker_get_wall_time_str(marker_detector_t *md, float timestamp_ms, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* MARKER_INTERNAL_H */
