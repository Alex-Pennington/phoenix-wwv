/* wwv_clock_report(): when a CLOCK line is said, and what it says without the
 * second-marking edges or a confirmed date. The chain end to end, with edges and a
 * known anchor error, is tests/clock_synth.sh.
 *
 * Checks:
 *   - nothing before the time of day is CONFIRMED (NONE, DECODED, PREDICTED), and
 *     nothing without an anchor;
 *   - one line per CONFIRMED frame: not again for the same frame and value, again
 *     for a new frame or a changed value;
 *   - with no sync detector (no edges) the frame start is used as it is and sigma is
 *     WWV_CLOCK_NO_FIT_SIGMA_MS;
 *   - utc_minus_anchor_s with a CONFIRMED date, and without one (the host's day,
 *     also across midnight);
 *   - band_khz null when not known; wwv_clock_unix_day against known dates. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "wwv_clock_report.h"

static int fails;
static void check(int ok, const char *what) {
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static double field(const char *line, const char *key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    return p ? strtod(p + strlen(pat), NULL) : NAN;
}

int main(void) {
    check(wwv_clock_unix_day(1970, 1) == 0.0, "1970-001 is 0");
    check(wwv_clock_unix_day(2026, 325) == 1795219200.0, "2026-325 (11-21) is 1795219200");
    check(wwv_clock_unix_day(2000, 61) == 951868800.0, "2000-061 (03-01, leap year) is 951868800");
    check(wwv_clock_unix_day(2028, 366) == 1861833600.0, "2028-366 (12-31) is 1861833600");

    /* sample 0 at 12:00:59.5 on the host's clock; the decoded 12:02 starts at stream
     * 60.5 s + 60 s, so a right host clock gives 0 */
    wwv_clock_anchor_t a = { .set = true, .in_sample0_unix = 1795262400.0 - 0.5 + 0.125,
                             .group_delay_ms = 1.5, .band_khz = 0.0 };
    snprintf(a.source, sizeof a.source, "wwv_live");
    wwv_td_result_t r = {0};
    r.frame_start_ms = 120500.0 + 1.5;      /* detector time: input stream time + group delay */
    r.hour = 12; r.minute = 2;
    wwv_clock_report_t st = {0};
    char buf[512];

    wwv_td_state_t not_yet[] = { WWV_TD_NONE, WWV_TD_DECODED, WWV_TD_PREDICTED };
    for (int i = 0; i < 3; i++) {
        r.tod_state = not_yet[i];
        check(wwv_clock_report(&st, &a, NULL, &r, 1.0, buf, sizeof buf) == 0, "no line before CONFIRMED");
    }
    r.tod_state = WWV_TD_CONFIRMED;
    wwv_clock_anchor_t none = a; none.set = false;
    check(wwv_clock_report(&st, &none, NULL, &r, 1.0, buf, sizeof buf) == 0, "no line without an anchor");

    /* no date: the host's day; host clock 125 ms ahead */
    r.date_state = WWV_TD_DECODED;
    check(wwv_clock_report(&st, &a, NULL, &r, 1795262600.25, buf, sizeof buf) > 0, "a line at CONFIRMED");
    printf("%s", buf);
    { const char *p = "CLOCK {\"confirmed\":true,"; check(strncmp(buf, p, strlen(p)) == 0, "prefix"); }
    check(fabs(field(buf, "utc_minus_anchor_s") + 0.125) < 1e-6, "utc_minus_anchor_s -0.125 (host day)");
    check(fabs(field(buf, "frame_stream_ms") - 120500.0) < 1e-3, "frame_stream_ms is detector time less the group delay");
    check(fabs(field(buf, "sigma_ms") - WWV_CLOCK_NO_FIT_SIGMA_MS) < 0.051, "no edges: the gate's sigma");
    check(strstr(buf, "\"band_khz\":null,") != NULL, "band_khz null when not known");
    check(fabs(field(buf, "t_unix") - 1795262600.25) < 1e-3, "t_unix as given");
    check(strstr(buf, "\"source\":\"wwv_live\"}") != NULL, "source");
    check(wwv_clock_report(&st, &a, NULL, &r, 1.0, buf, sizeof buf) == 0, "not again for the same frame");
    r.minute = 3;
    check(wwv_clock_report(&st, &a, NULL, &r, 1.0, buf, sizeof buf) > 0, "again when the value changes");
    check(fabs(field(buf, "utc_minus_anchor_s") - 59.875) < 1e-6, "a value a minute on reads +60 s");

    /* the next frame, date CONFIRMED (2026-325), band known */
    r.minute = 3; r.frame_start_ms += 60000.0;
    r.date_state = WWV_TD_CONFIRMED; r.year = 2026; r.doy = 325;
    a.band_khz = 10000.0;
    check(wwv_clock_report(&st, &a, NULL, &r, 1.0, buf, sizeof buf) > 0, "again for a new frame");
    check(fabs(field(buf, "utc_minus_anchor_s") + 0.125) < 1e-6, "utc_minus_anchor_s -0.125 (confirmed date)");
    check(strstr(buf, "\"band_khz\":10000,") != NULL, "band_khz given");
    /* a confirmed date a day off is believed: the host's clock is the one tested */
    r.doy = 326; r.frame_start_ms += 60000.0; r.minute = 4;
    check(wwv_clock_report(&st, &a, NULL, &r, 1.0, buf, sizeof buf) > 0, "a frame on");
    check(fabs(field(buf, "utc_minus_anchor_s") - (86400.0 - 0.125)) < 1e-6, "the confirmed date is used");

    /* across midnight without a date: host 23:59:59.9 at the decoded 00:00 start */
    wwv_clock_anchor_t m = a;
    m.in_sample0_unix = 1795219200.0 + 86400.0 - 0.1 - 10.0;   /* 2026-11-22 00:00 is 1795305600 */
    wwv_clock_report_t st2 = {0};
    wwv_td_result_t r2 = {0};
    r2.tod_state = WWV_TD_CONFIRMED; r2.hour = 0; r2.minute = 0;
    r2.frame_start_ms = 10000.0 + 1.5;
    check(wwv_clock_report(&st2, &m, NULL, &r2, 1.0, buf, sizeof buf) > 0, "midnight line");
    check(fabs(field(buf, "utc_minus_anchor_s") - 0.1) < 1e-6, "midnight: the next day's 00:00, +0.1 s");

    printf("%s test_clock_report\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
