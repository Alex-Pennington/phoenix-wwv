/**
 * @file wwv.h
 * @brief Phoenix WWV Detection Library - Unified Header
 *
 * Single include for all WWV time signal detection capabilities.
 *
 * @author Phoenix Nest LLC / Alex Pennington (KY4OLB)
 * @date 2024-12
 */

#ifndef WWV_H
#define WWV_H

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Version
 *============================================================================*/

#define WWV_VERSION_MAJOR   0
#define WWV_VERSION_MINOR   1
#define WWV_VERSION_PATCH   0
#define WWV_VERSION_STRING  "0.1.0"

/*============================================================================
 * Core Detectors
 *============================================================================*/

#include "tick_detector.h"
#include "marker_detector.h"
#include "sync_detector.h"

/*============================================================================
 * Correlation / Comb Filtering
 *============================================================================*/

#include "tick_correlator.h"
#include "marker_correlator.h"
#include "tick_comb_filter.h"
#include "slow_marker_detector.h"

/*============================================================================
 * Channel Processing
 *============================================================================*/

#include "channel_filters.h"
#include "tone_tracker.h"

/*============================================================================
 * BCD Time Code Decoding
 *============================================================================*/

#include "bcd_envelope.h"
#include "bcd_decoder.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"

/*============================================================================
 * FFT Support
 *============================================================================*/

#include "kiss_fft.h"

/*============================================================================
 * Constants
 *============================================================================*/

/* WWV/WWVH frequencies */
#define WWV_TICK_FREQ_HZ        1000    /* WWV tick tone */
#define WWVH_TICK_FREQ_HZ       1200    /* WWVH tick tone */
#define WWV_HOUR_FREQ_HZ        1500    /* Hour marker (both stations) */
#define WWV_BCD_SUBCARRIER_HZ   100     /* BCD time code subcarrier */

/* Tick/Marker durations */
#define WWV_TICK_DURATION_MS    5       /* Regular second tick */
#define WWV_MARKER_DURATION_MS  800     /* Minute/hour marker */

/* BCD pulse widths */
#define WWV_BCD_ZERO_MS         200     /* Binary 0 */
#define WWV_BCD_ONE_MS          500     /* Binary 1 */
#define WWV_BCD_MARKER_MS       800     /* Position marker */

/* Protected zone (no modulation) */
#define WWV_PROTECTED_ZONE_MS   40      /* 10ms before + 5ms tick + 25ms after */

/* Reference tones */
#define WWV_TONE_500_HZ         500
#define WWV_TONE_600_HZ         600
#define WWV_TONE_440_HZ         440     /* Musical A (minute 2) */

/*============================================================================
 * Sync States
 *============================================================================*/

typedef enum {
    WWV_SYNC_SEARCHING = 0,     /* Looking for first marker */
    WWV_SYNC_ACQUIRING,         /* Building confidence */
    WWV_SYNC_LOCKED,            /* Full synchronization */
    WWV_SYNC_RECOVERING         /* Lost marker, trying to reacquire */
} wwv_sync_state_t;

/*============================================================================
 * Decoded Time Structure
 *============================================================================*/

typedef struct {
    int hour;           /* 0-23 UTC */
    int minute;         /* 0-59 */
    int second;         /* 0-59 */
    int day_of_year;    /* 1-366 */
    int year;           /* 2-digit year */
    int dut1_tenths;    /* DUT1 correction in 0.1s units */
    bool dut1_positive; /* DUT1 sign */
    bool leap_second;   /* Leap second warning */
    bool dst_change;    /* DST change pending */
    bool valid;         /* Time decode successful */
} wwv_time_t;

#ifdef __cplusplus
}
#endif

#endif /* WWV_H */
