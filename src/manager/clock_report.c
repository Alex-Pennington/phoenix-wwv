/**
 * @file clock_report.c
 * @brief The CLOCK line (wwv_clock_report.h)
 *
 * The minute's start on the detector clock is the decoder's frame start (the
 * sync detector's held phase at that minute) moved by where the frame's own
 * second-marking edges put the seconds (sync_detector_phase_fit, each edge less
 * its known lead PHASE_LEAD_*). The held phase alone can sit up to the tracker's
 * dead band (8 ms on ticks, 15 ms on BCD starts) off those edges, since it slews
 * only beyond it. The front-end group delay turns detector time into input
 * stream time, which is what the anchor dates.
 *
 * sigma_ms combines, as independent errors, the fit's standard error (the
 * median of the edges' offsets: 1.2533 x robust SD / sqrt n) and how well the
 * edges' lead is known (3 ms for ticks, 10 ms for BCD starts; sync_detector.c
 * PHASE_LEAD_SIGMA_*). With fewer than 4 edges in the gate it is the gate's
 * uniform SD instead. It does not cover the anchor's own delivery latency, the
 * radio path, or the sample clock's rate error since the anchor: those are
 * biases, documented with the line.
 */
#include "wwv_clock_report.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* The fit's window: the frame's seconds 0..59, each +-500 ms */
#define FIT_LEAD_MS     500.0
#define MINUTE_MS       60000.0

double wwv_clock_unix_day(int year, int doy) {
    /* days from 1970-01-01 to Jan 1 of year (Howard Hinnant's days_from_civil, m = 1, d = 1) */
    long y = (long)year - 1;            /* January: the era's year starts in March */
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + 306;   /* Jan 1 is day 306 of a March year */
    long days = era * 146097 + doe - 719468;
    return (double)(days + doy - 1) * 86400.0;
}

int wwv_clock_report(wwv_clock_report_t *st, const wwv_clock_anchor_t *a, sync_detector_t *sd,
                     const wwv_td_result_t *r, double t_unix, char *buf, size_t n) {
    if (!st || !a || !a->set || !r || !buf || n == 0) return 0;
    if (r->tod_state != WWV_TD_CONFIRMED) return 0;
    if (st->said && r->frame_start_ms == st->frame_start_ms &&
        r->hour == st->hour && r->minute == st->minute)
        return 0;

    sync_phase_fit_t f = sync_detector_phase_fit(sd, r->frame_start_ms, r->frame_start_ms - FIT_LEAD_MS,
                                                 r->frame_start_ms + MINUTE_MS - FIT_LEAD_MS);
    double start_det_ms = r->frame_start_ms;
    double sigma;
    if (f.n >= 4) {
        start_det_ms += f.offset_ms;
        sigma = sqrt((double)f.sigma_ms * f.sigma_ms + (double)f.lead_sigma_ms * f.lead_sigma_ms);
    } else {
        sigma = WWV_CLOCK_NO_FIT_SIGMA_MS;
    }
    double frame_stream_ms = start_det_ms - a->group_delay_ms;
    double host = a->in_sample0_unix + frame_stream_ms / 1000.0;

    double tod = r->hour * 3600.0 + r->minute * 60.0, utc;
    if (r->date_state == WWV_TD_CONFIRMED) {
        utc = wwv_clock_unix_day(r->year, r->doy) + tod;
    } else {
        /* the day is the host's: the one that puts the decoded minute nearest it */
        utc = floor((host - tod) / 86400.0 + 0.5) * 86400.0 + tod;
    }

    char band[24];
    if (a->band_khz > 0.0) snprintf(band, sizeof band, "%g", a->band_khz);
    else snprintf(band, sizeof band, "null");
    int k = snprintf(buf, n,
                     "CLOCK {\"confirmed\":true,\"utc_minus_anchor_s\":%.6f,\"sigma_ms\":%.1f,\"band_khz\":%s,"
                     "\"t_unix\":%.3f,\"frame_stream_ms\":%.3f,\"source\":\"%s\"}\n",
                     utc - host, sigma, band, t_unix, frame_stream_ms, a->source);
    if (k <= 0 || (size_t)k >= n) return 0;
    st->said = true;
    st->frame_start_ms = r->frame_start_ms;
    st->hour = r->hour;
    st->minute = r->minute;
    return k;
}
