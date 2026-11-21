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
 * from its own reserved bits, which are always 0, against a prior. Erasures and
 * mid-second detections carry no evidence. There are two models: nominal (the
 * measured rates) and pessimistic (rates the worst measured air run stays well
 * inside, raised to what the chain's own evidence shows: p01 from the reserved
 * seconds, p10 from the P seconds read short, pooled over the history; never below
 * the configured rates). The margin of a result is the log-likelihood ratio
 * (nats) between the best hypothesis and the best one that differs in that part.
 *
 * Time of day, date, DUT1, DST bits and the leap second warning each get a state:
 *   NONE       no hypothesis leads by decoded_nats, or only one frame has evidence
 *   DECODED    leads by decoded_nats: the best reading, not verified
 *   CONFIRMED  leads by confirmed_nats under both the nominal and the pessimistic
 *              model with the same value, on confirm_looks consecutive minutes of
 *              the chain, each stepping from the one before; the previous minute of the same chain was DECODED
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

/* Error model, from the symbols whose pulse starts within 250 ms of the second on the
 * four air runs (KiwiSDR and RSP2, 5 MHz, plain and AGC; tests/air/decode_score.py
 * prints the counts):
 *   sent 0 read as 1  8/1028 pooled (0.78%); worst run 6/183 (3.3%, 95% upper 6.4%)
 *   sent 1 read as 0  8/383 pooled (2.1%); worst run 7/103 (6.8%, 95% upper 12.4%)
 *   sent P read as 1  10/157 pooled (6.4%); worst run 5/42 (11.9%): a long pulse
 *                     read one class short, the mechanism behind a 1 read as 0
 * Nominal: the pooled rates (p01 0.78% -> 0.01; p10 = 1 -> 0 and P -> 1 pooled,
 * 18/540 = 3.3% -> 0.03). Pessimistic p01: 0.07, at least twice the worst run and
 * above its 95% upper bound (2 x 3.3%, 6.4%).
 * Pessimistic p10: 0.10. That is above the worst run's 6.8% but below twice it
 * (13.6%) and below its 95% upper bound (12.4%), so the floor alone does not cover
 * that run's worst case. What 0.10 rests on is the chain's own estimate (below) and
 * these measurements, 0 CONFIRMED wrong in each (tests/sim_wwv_time_decoder.c,
 * 5000 chains of 30 minutes; the CTest Slow cases): two and three times the worst
 * air run; the live run's rates and twice them; a sent 1 read as 0 at 12.4% with
 * every P read right, so the P seconds raise nothing; and 1 -> 0 stepping from 2%
 * to 12.4% at minute 4, 8, 12 or 15, so the chain's estimate lags the step. And
 * the 17 live replays of 2026-11-20/21 (tests/air/replay_events): 0 wrong. The cost
 * of 0.15 was one or more minutes to CONFIRMED on most of them (NOTES.md).
 * Prior weight: the 0 -> 1 rate varies between runs as a Beta prior of weight 66
 * would; 50 leaves room for more variation between frames than between runs.
 *
 * The pessimistic rates are floors, not the model: the live RSP2 run of 2026-11-20
 * (90 minutes) read a sent 0 as 1 in 14.4%, a 1 as 0 in 15.0%, and P as 1 or 0 in
 * 20.9%, with 0 -> 1 between 10.7% and 21.4% per 10-minute block. A model that
 * trusts a symbol more than the signal deserves lowers the effective CONFIRMED
 * margin: at live rates a fixed 0.07 / 0.15 gave about 9 nats of ln 10^6, at
 * twice them 4. So each chain raises them to its own evidence: the reserved
 * seconds sample 0 -> 1 without bias (14.3% live, 14.4% at the data seconds), and
 * a P read short is the 1 -> 0 failure on a longer pulse: P read 1 or 0 against
 * 1 -> 0 is 20.9% / 15.0% live, 9.3% / 5.8% saga plain, 2.3% / 0% kiwi plain,
 * 0% / 0% kiwi agc and 0/34 / 1/87 saga agc, where the floor covers it. The prior
 * weight is the same for both. */
#define WWV_TD_DEFAULT_P01          0.01f
#define WWV_TD_DEFAULT_P10          0.03f
#define WWV_TD_DEFAULT_P01_PESSIMISTIC 0.07f
#define WWV_TD_DEFAULT_P10_PESSIMISTIC 0.10f
#define WWV_TD_DEFAULT_P01_PRIOR_WEIGHT 50.0f
#define WWV_TD_DEFAULT_DECODED      2.302585f   /* ln 10 */
/* CONFIRMED: the margin is checked every minute, so over a chain a wrong value can
 * lead once by chance. Even with an exact model, a single-bit alternative ever leads
 * by L nats with probability about e^-L, and a chain has about 38 of them. In
 * tests/sim_wwv_time_decoder.c (5000 chains of 30 minutes at three times the worst
 * run's rates), ln 10^4 confirmed wrong in 11 chains, ln 10^5 in 1, ln 10^6 in none
 * (three seeds). Requiring the margin on 2 or 3 consecutive minutes left 7 and 4 at
 * ln 10^4 and 1 at ln 10^5, at about the same cost in time, so confirm_looks is 1.
 * With the pessimistic rates raised from the chain's evidence, ln 10^5 still
 * confirmed wrong in 1 of 5000 chains (60 minutes) at 1.5 times the live rates. */
#define WWV_TD_DEFAULT_CONFIRMED    13.815511f  /* ln 10^6 */
#define WWV_TD_DEFAULT_CONFIRM_LOOKS 1
#define WWV_TD_DEFAULT_BOUNDARY_MS  250.0f
#define WWV_TD_MAX_FRAMES           60

typedef struct {
    float p01;              /* prior mean rate of a sent 0 read as 1 */
    float p10;              /* rate of a sent 1 read as 0 */
    float p01_pessimistic;  /* pessimistic model: prior mean and floor of p01, which
                               rises with the chain's reserved seconds read 1 */
    float p10_pessimistic;  /* pessimistic model: prior mean and floor of p10, which
                               rises with the chain's P seconds read 1 or 0.
                               CONFIRMED must hold under both models */
    float p01_prior_weight; /* pseudo-observations behind each p01 prior, and behind
                               the pessimistic p10 prior */
    float decoded_nats;     /* margin for DECODED */
    float confirmed_nats;   /* margin for CONFIRMED */
    int confirm_looks;      /* consecutive minutes the CONFIRMED margin must hold */
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
    float p01;                  /* this frame's estimated 0 -> 1 rate, nominal model */
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

    /* The pessimistic model's margin for each part: CONFIRMED needs it to be at
     * least confirmed_nats as well as the nominal margin. Negative (minus its
     * margin) when that model's best value differs from the nominal one. */
    float tod_pmargin, date_pmargin, dut1_pmargin, dst_pmargin, lsw_pmargin;
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

/**
 * Drop the open frame without decoding it: its symbols span a sync phase break
 * (some were binned under a phase since found wrong). The chain is kept.
 */
void wwv_time_decoder_discard_frame(wwv_time_decoder_t *td);

/**
 * Drop the open frame and the chain: sync found its phase a whole second off
 * (sync_detector_get_phase_resets), so frames closed before that was seen may
 * hold every symbol in the wrong second. Decoding starts again from the next
 * frame.
 */
void wwv_time_decoder_reset(wwv_time_decoder_t *td);

/** Adapter for bcd_correlator_set_callback(): user_data is the decoder. A symbol
 *  of a different phase epoch from the open frame's discards that frame. */
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
