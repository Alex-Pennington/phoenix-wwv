/**
 * @file wwv_clock_report.h
 * @brief The CLOCK line: the host clock's error, read off a CONFIRMED time of day
 *
 * A consumer that disciplines or checks the host clock needs, once the decoder has
 * CONFIRMED the time of day, how far the host's reckoning of a minute's start sits
 * from WWV's. wwv_live prints it on stdout and in its --log, and
 * tests/air/replay_events prints it when it has an anchor:
 *
 *   CLOCK {"confirmed":true,"utc_minus_anchor_s":S,"sigma_ms":E,"band_khz":K|null,
 *          "t_unix":T,"frame_stream_ms":F,"source":"SRC"}
 *
 * F is the input stream time (ms since input sample 0) of the minute's start, as the
 * second-marking edges place it; S = UTC(decoded minute start) - (anchor + F/1000).
 * docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md and docs/WWV_LIVE.md define each field and
 * what S does and does not contain.
 */
#ifndef WWV_CLOCK_REPORT_H
#define WWV_CLOCK_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include "sync_detector.h"
#include "wwv_time_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool set;
    double in_sample0_unix;     /* host clock (UNIX s) of input sample 0 */
    double group_delay_ms;      /* front-end group delay: detector time t is input stream t - this */
    double band_khz;            /* WWV carrier, kHz; <= 0 when not known */
    char source[32];            /* where the anchor came from: rsp2_stream, wwv_live, t0 */
} wwv_clock_anchor_t;

typedef struct {
    bool said;
    double frame_start_ms;      /* the frame of the last line (detector time) */
    int hour, minute;           /* its confirmed time of day */
} wwv_clock_report_t;

/* Without enough edges in the frame's minute, the held phase is used as it is:
 * it may sit anywhere in the tracker's +-40 ms gate, so sigma is the gate's
 * uniform SD (40 / sqrt 3 ms) */
#define WWV_CLOCK_NO_FIT_SIGMA_MS 23.1f

/**
 * On a decoder result: when its time of day is CONFIRMED and this is the first
 * such result, a new frame, or a different confirmed value, format the CLOCK line
 * (with its "CLOCK " prefix and a newline) into buf and return its length; else
 * return 0. Nothing is formatted before CONFIRMED or without an anchor.
 * t_unix is the host time to report as now.
 */
int wwv_clock_report(wwv_clock_report_t *st, const wwv_clock_anchor_t *a, sync_detector_t *sd,
                     const wwv_td_result_t *r, double t_unix, char *buf, size_t n);

/** UNIX time of 00:00 UTC on day doy (1-based) of year (proleptic Gregorian) */
double wwv_clock_unix_day(int year, int doy);

#ifdef __cplusplus
}
#endif

#endif
