/**
 * @file wwv_time_decoder.h
 * @brief WWV/WWVH 100 Hz time code frame decoder: UTC time and date from BCD symbols
 *
 * Frame format: NIST SP 432 (2002), Table 3.13 "WWV and WWVH time code bits".
 * One frame per minute, describing the minute in which it is sent. BCD fields are
 * sent least significant bit first:
 *
 *   second  0      frame reference: no pulse
 *           1      reserved (0)
 *           2      DST indicator, changes 24 h after second 55
 *           3      leap second warning (a leap second at the end of this month)
 *           4-7    year units (1, 2, 4, 8)
 *           8      reserved
 *           9      P1
 *           10-13  minute units (1, 2, 4, 8)     14 reserved
 *           15-17  minute tens (10, 20, 40)      18 reserved
 *           19     P2
 *           20-23  hour units (1, 2, 4, 8)       24 reserved
 *           25-26  hour tens (10, 20)            27-28 reserved
 *           29     P3
 *           30-33  day of year units             34 reserved
 *           35-38  day of year tens (10..80)
 *           39     P4
 *           40-41  day of year hundreds (100, 200)
 *           42-48  reserved
 *           49     P5
 *           50     UT1 sign (1 = positive)
 *           51-54  year tens (10, 20, 40, 80)
 *           55     DST indicator, changes at 00:00 UTC on the day of a change
 *           56-58  UT1 correction magnitude (0.1, 0.2, 0.4 s)
 *           59     P0
 *
 * WWV has no parity. Each symbol is evidence about one bit of a hypothesis, and
 * consecutive minutes differ by exactly one minute. The decoder keeps the symbols of
 * up to max_frames consecutive minutes (minute starts a whole number of minutes
 * apart on the sync anchor) and scores every UTC minute from 2000 to 2099 against
 * all of them at once, under a two-rate error model: a sent 0 is read as 1 with
 * probability p01, a sent 1 as 0 with probability p10. A frame's p01 is estimated
 * from its own reserved bits, which are always 0. Erasures and mid-second
 * detections carry no evidence. The margin of a result is the log-likelihood ratio
 * (nats) between the best hypothesis and the best one that differs in that part.
 *
 * Time of day, date, DUT1, DST bits and the leap second warning each get a state:
 *   NONE       no hypothesis leads by decoded_nats, or only one frame has evidence
 *   DECODED    leads by decoded_nats: the best reading, not verified
 *   CONFIRMED  leads by confirmed_nats under both p10 and p10_pessimistic with
 *              the same value; the previous minute of the same chain was DECODED
 *              or better with the value this one steps from; the history holds
 *              two P markers; for the date and status bits, the time of day is
 *              reported this minute
 *   PREDICTED  not CONFIRMED now; the last CONFIRMED value in this chain carried
 *              forward by the elapsed minutes, with no DECODED reading against it
 * A decode that disagrees with the carried CONFIRMED value is a conflict: it is
 * counted, the carried value is dropped, and the new reading is at most DECODED.
 */

#ifndef WWV_TIME_DECODER_H
#define WWV_TIME_DECODER_H

#include <stdbool.h>
#include "bcd_correlator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*============================================================================
 * Configuration
 *============================================================================*/

/* Error model defaults, measured on the four air runs (KiwiSDR and RSP2, 5 MHz):
 * a symbol whose pulse starts at its second reads a sent 0 as 1 in 125 of 493 and
 * a sent 1 as 0 in 3 of 184 (tests/air/decode_score.py prints the counts). */
#define WWV_TD_DEFAULT_P01          0.25f
#define WWV_TD_DEFAULT_P10          0.05f       /* above the 95% bound of 3/184 */
#define WWV_TD_DEFAULT_P10_PESSIMISTIC 0.15f
#define WWV_TD_DEFAULT_DECODED      2.302585f   /* ln 10 */
#define WWV_TD_DEFAULT_CONFIRMED    9.210340f   /* ln 10^4 */
#define WWV_TD_DEFAULT_BOUNDARY_MS  250.0f
#define WWV_TD_MAX_FRAMES           60

typedef struct {
    float p01;              /* prior rate of a sent 0 read as 1 */
    float p10;              /* rate of a sent 1 read as 0 */
    float p10_pessimistic;  /* CONFIRMED must also hold with this p10 */
    float decoded_nats;     /* margin for DECODED */
    float confirmed_nats;   /* margin for CONFIRMED */
    float boundary_ms;      /* a symbol whose pulse starts later than this after its
                               second is a mid-second detection: not evidence */
    int max_frames;         /* minutes of history, 2..WWV_TD_MAX_FRAMES */
} wwv_td_config_t;

/*============================================================================
 * Types
 *============================================================================*/

typedef enum {
    WWV_TD_NONE = 0,
    WWV_TD_DECODED,
    WWV_TD_CONFIRMED,
    WWV_TD_PREDICTED
} wwv_td_state_t;

typedef struct {
    float frame_start_ms;   /* the minute's start on the detector clock (sync anchor) */
    int chain_minute;       /* minutes since the chain's first frame */
    int chain_frames;       /* frames in the evidence history */
    int observations;       /* 0/1 symbols used as evidence in the history */

    /* This minute's frame, one character per second:
     *   0 1 P  symbol whose pulse starts at the second
     *   ~      a 0 or 1 from a mid-second detection only (not used)
     *   .      erasure: locked, no BCD event
     *   _      no symbol: not locked */
    char frame[61];
    int markers_seen;           /* P at 9, 19, 29, 39, 49, 59 */
    int markers_contradicted;   /* 0 at those seconds */
    int reserved_ones;          /* 1 at a reserved second (always sent as 0) */
    int reserved_zeros;
    int bcd_violations;         /* fields whose observed 1s make an impossible value
                                   (digit > 9, minute > 59, hour > 23, day > 366):
                                   those 1s are not used */
    float p01;                  /* this frame's estimated 0 -> 1 rate */
    bool admitted;              /* used as evidence: no more 0s at the P seconds (or Ps
                                   elsewhere) than Ps, and no marker at second 0 */
    bool hard_valid;            /* every bit observed, reserved bits 0, digits and
                                   ranges valid, all six markers: decodable alone */

    wwv_td_state_t tod_state;
    int hour, minute;
    float tod_margin;

    wwv_td_state_t date_state;
    int year, doy, month, mday;
    float date_margin;

    wwv_td_state_t dut1_state;
    int dut1_tenths;            /* signed UT1 - UTC, 0.1 s units */
    float dut1_margin;

    wwv_td_state_t dst_state;
    int dst55, dst2;            /* second 55 changes at 00:00 UTC on a change day,
                                   second 2 24 h later; both 0 = standard time */
    float dst_margin;

    wwv_td_state_t lsw_state;
    int lsw;
    float lsw_margin;

    int conflicts;              /* decodes against a carried CONFIRMED value, total */
} wwv_td_result_t;

typedef void (*wwv_td_callback_fn)(const wwv_td_result_t *result, void *user_data);

typedef struct wwv_time_decoder wwv_time_decoder_t;

/*============================================================================
 * API
 *============================================================================*/

/** Defaults: the WWV_TD_DEFAULT_* values and WWV_TD_MAX_FRAMES. */
void wwv_td_default_config(wwv_td_config_t *cfg);

/** Create a decoder. cfg may be NULL for the defaults. */
wwv_time_decoder_t *wwv_time_decoder_create(const wwv_td_config_t *cfg);

void wwv_time_decoder_destroy(wwv_time_decoder_t *td);

/** Called once per minute, when that minute's frame is complete. */
void wwv_time_decoder_set_callback(wwv_time_decoder_t *td, wwv_td_callback_fn cb, void *user_data);

/**
 * Add one second's symbol.
 * @param minute_start_ms  start of the minute the second belongs to (sync anchor)
 * @param second           0..59
 * @param symbol           '0', '1', 'P', or '.' for an erasure
 * @param pulse_offset_ms  start of the pulse from the start of the second; NAN if none
 */
void wwv_time_decoder_symbol(wwv_time_decoder_t *td, float minute_start_ms, int second,
                             char symbol, float pulse_offset_ms);

/** Adapter for bcd_correlator_set_callback(): user_data is the decoder. */
void wwv_time_decoder_bcd_callback(const bcd_symbol_event_t *event, void *user_data);

/**
 * Close the current frame once its last window can no longer arrive (frame start
 * + 62 s). Call it from the same loop as bcd_correlator_periodic_check().
 */
void wwv_time_decoder_periodic(wwv_time_decoder_t *td, float now_ms);

/** The last minute's result, or NULL before the first frame closes. */
const wwv_td_result_t *wwv_time_decoder_get_last(const wwv_time_decoder_t *td);

const char *wwv_td_state_name(wwv_td_state_t state);

/**
 * Encode one frame per NIST SP 432 Table 3.13.
 * @param frame  receives 60 characters ('0', '1', 'P'; second 0 is '.') and a NUL
 * @return false if a field is out of range
 */
bool wwv_td_encode(int year, int doy, int hour, int minute, int dut1_tenths,
                   int dst55, int dst2, int lsw, char frame[61]);

/** Day of year to month and day. */
void wwv_td_doy_to_date(int year, int doy, int *month, int *mday);

#ifdef __cplusplus
}
#endif

#endif /* WWV_TIME_DECODER_H */
