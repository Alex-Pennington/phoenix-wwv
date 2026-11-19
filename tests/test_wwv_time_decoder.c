/* WWV time decoder tests.
 *
 * 1. Encoder against frames written out by hand from NIST SP 432 Table 3.13: the
 *    SP 432 worked example (2001, day 173, 21:10, UT1 +0.3 s) and the air capture's
 *    minute (2026-11-19 = day 323, 12:19, UT1 -0.1 s), the latter as score.py builds it.
 * 2. Clean minutes: 2026-323 12:10 on; a day-of-year rollover; a year boundary; the
 *    end of a leap year (day 366); DST bits on a change day and in summer; a leap
 *    second warning with UT1 +0.7 s; UT1 -0.7 s. Every reported part must be right
 *    in every state, and each run must end with every part CONFIRMED.
 * 3. Corrupted: one frame alone; a burst of bit errors in a clean chain; half the
 *    seconds erased; a frame with an invalid BCD digit; a chain on the wrong second
 *    phase (anchor 1 s late). None may report a wrong value in any state.
 *    Then random chains with symbol errors at the worst measured air run and three
 *    times it (sent 0 read as 1 at 3.3% and 10%, sent 1 read as 0 at 6.8% and 20%,
 *    sent P read as 1 at 12% and 36%), 45% erasures and 8%
 *    mid-second detections, from random minutes in 2000-2099: no part may be
 *    CONFIRMED wrong. Wrong DECODED parts are counted and printed: DECODED is the
 *    unverified state.
 * 4. WWVB: frames in the WWVB layout (SP 432 Table 2.3), with and without the
 *    second-0 marker. Nothing may be CONFIRMED, and the WWVB time of day must never
 *    be reported.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "wwv_time_decoder.h"

static int fails;
static void check(int ok, const char *fmt, const char *name) {
    if (!ok) { printf("FAIL %s: %s\n", name, fmt); fails++; }
}

/*---------------------------------------------------------------- calendar */
typedef struct { int year, doy, hour, minute; } utc_t;
static int ylen(int y) { return (y % 4 == 0) ? 366 : 365; }
static utc_t add_minutes(utc_t t, int n) {
    while (n-- > 0) {
        if (++t.minute == 60) { t.minute = 0;
            if (++t.hour == 24) { t.hour = 0;
                if (++t.doy > ylen(t.year)) { t.doy = 1; t.year++; } } }
    }
    return t;
}

/*---------------------------------------------------------------- result capture */
typedef struct { int dut1, dst55, dst2, lsw; } status_t;
static utc_t g_truth;
static status_t g_status;
static int g_wrong_any, g_wrong_conf, g_wrong_dec, g_results;
static wwv_td_result_t g_last;
static const char *g_name;
static int g_quiet;

static void judge(const char *part, wwv_td_state_t st, int ok) {
    if (st == WWV_TD_NONE || ok) return;
    g_wrong_any++;
    if (st == WWV_TD_CONFIRMED) g_wrong_conf++;
    if (st == WWV_TD_DECODED) g_wrong_dec++;
    if (!g_quiet) printf("     %s: %s %s wrong at %04d-%03d %02d:%02d\n", g_name, part,
                         wwv_td_state_name(st), g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute);
}

static void on_result(const wwv_td_result_t *r, void *u) {
    (void)u;
    g_results++;
    g_last = *r;
    judge("time of day", r->tod_state, r->hour == g_truth.hour && r->minute == g_truth.minute);
    judge("date", r->date_state, r->year == g_truth.year && r->doy == g_truth.doy);
    judge("DUT1", r->dut1_state, r->dut1_tenths == g_status.dut1);
    judge("DST", r->dst_state, r->dst55 == g_status.dst55 && r->dst2 == g_status.dst2);
    judge("LSW", r->lsw_state, r->lsw == g_status.lsw);
}

static void reset_capture(const char *name) {
    g_name = name;
    g_wrong_any = g_wrong_conf = g_wrong_dec = g_results = 0;
    memset(&g_last, 0, sizeof g_last);
}

/* Feed one minute. Characters: 0 1 P as symbols at the second, '.' erasure,
 * '~' a 0 from a mid-second detection, '_' no symbol. */
static void feed(wwv_time_decoder_t *td, float start_ms, const char *f) {
    for (int s = 0; s < 60; s++) {
        char c = f[s];
        if (c == '_') continue;
        if (c == '.') wwv_time_decoder_symbol(td, start_ms, s, '.', NAN);
        else if (c == '~') wwv_time_decoder_symbol(td, start_ms, s, '0', 520.0f);
        else wwv_time_decoder_symbol(td, start_ms, s, c, 20.0f);
    }
    wwv_time_decoder_periodic(td, start_ms + 62500.0f);
}

/*---------------------------------------------------------------- 1. encoder */
static void test_encoder(void) {
    char f[61];
    /* SP 432 p. 48 example; DST and leap second bits 0. Written from Table 3.13. */
    const char *ex2001 = ".00010000P" "000001000P" "100000100P" "110001110P" "100000000P" "100000110P";
    wwv_td_encode(2001, 173, 21, 10, 3, 0, 0, 0, f);
    check(strcmp(f, ex2001) == 0, "2001-173 21:10 UT1 +0.3 frame", "encoder");
    if (strcmp(f, ex2001)) printf("     got  %s\n     want %s\n", f, ex2001);
    /* score.py's frame for 2026-11-19 12:19, DUT1 -0.1 s (second 0 printed as '.') */
    const char *air = ".00001100P100101000P010001000P110000100P110000000P001000100P";
    wwv_td_encode(2026, 323, 12, 19, -1, 0, 0, 0, f);
    check(strcmp(f, air) == 0, "2026-323 12:19 UT1 -0.1 frame", "encoder");
    if (strcmp(f, air)) printf("     got  %s\n     want %s\n", f, air);
    int m, d;
    wwv_td_doy_to_date(2026, 323, &m, &d);
    check(m == 11 && d == 19, "2026 day 323 is 11-19", "encoder");
    wwv_td_doy_to_date(2028, 366, &m, &d);
    check(m == 12 && d == 31, "2028 day 366 is 12-31", "encoder");
    check(!wwv_td_encode(2027, 366, 0, 0, 0, 0, 0, 0, f), "2027 has no day 366", "encoder");
    printf("%-4s %-58s\n", fails ? "FAIL" : "ok", "encoder matches SP 432 Table 3.13 by hand");
}

/*---------------------------------------------------------------- 2. clean */
static int clean_run(const char *name, utc_t t0, int minutes, status_t st) {
    wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
    wwv_time_decoder_set_callback(td, on_result, NULL);
    reset_capture(name);
    g_status = st;
    int first_conf = -1;
    char f[61];
    for (int k = 0; k < minutes; k++) {
        g_truth = add_minutes(t0, k);
        wwv_td_encode(g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute, st.dut1,
                      st.dst55, st.dst2, st.lsw, f);
        feed(td, 5000.0f + 60000.0f * k + (float)((k * 37) % 41 - 20), f);
        if (first_conf < 0 && g_last.tod_state == WWV_TD_CONFIRMED && g_last.date_state == WWV_TD_CONFIRMED)
            first_conf = k + 1;
    }
    int all = g_last.tod_state == WWV_TD_CONFIRMED && g_last.date_state == WWV_TD_CONFIRMED &&
              g_last.dut1_state == WWV_TD_CONFIRMED && g_last.dst_state == WWV_TD_CONFIRMED &&
              g_last.lsw_state == WWV_TD_CONFIRMED;
    int pass = all && g_wrong_any == 0 && g_results == minutes;
    printf("%-4s %-58s time+date CONFIRMED at minute %d, wrong %d\n", pass ? "ok" : "FAIL", name,
           first_conf, g_wrong_any);
    if (!pass) {
        fails++;
        printf("     last: tod %s date %s dut1 %s dst %s lsw %s, results %d/%d\n",
               wwv_td_state_name(g_last.tod_state), wwv_td_state_name(g_last.date_state),
               wwv_td_state_name(g_last.dut1_state), wwv_td_state_name(g_last.dst_state),
               wwv_td_state_name(g_last.lsw_state), g_results, minutes);
    }
    wwv_time_decoder_destroy(td);
    return first_conf;
}

/*---------------------------------------------------------------- 3. corrupted */
static unsigned rng = 12345u;
static double urand(void) { rng = rng * 1103515245u + 12345u; return ((rng >> 8) & 0xFFFFFF) / 16777216.0; }

static void corrupt(char *f, double p01, double p10, double pP1, double erase, double mid) {
    for (int s = 1; s < 60; s++) {
        double u = urand();
        if (u < erase) { f[s] = '.'; continue; }
        if (urand() < mid) { f[s] = '~'; continue; }
        if (f[s] == '0' && urand() < p01) f[s] = '1';
        else if (f[s] == '1' && urand() < p10) f[s] = '0';
        else if (f[s] == 'P' && urand() < pP1) f[s] = '1';
    }
}

typedef void (*mutate_fn)(char *f, int k);

static void corrupted_run(const char *name, utc_t t0, int minutes, mutate_fn mutate, int expect_none) {
    wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
    wwv_time_decoder_set_callback(td, on_result, NULL);
    reset_capture(name);
    g_status = (status_t){-1, 0, 0, 0};
    int reported = 0;
    char f[61];
    for (int k = 0; k < minutes; k++) {
        g_truth = add_minutes(t0, k);
        wwv_td_encode(g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute, -1, 0, 0, 0, f);
        mutate(f, k);
        feed(td, 5000.0f + 60000.0f * k, f);
        if (g_last.tod_state != WWV_TD_NONE || g_last.date_state != WWV_TD_NONE) reported++;
    }
    int pass = g_wrong_any == 0 && (!expect_none || reported == 0);
    printf("%-4s %-58s wrong %d, minutes with a time reported %d/%d\n", pass ? "ok" : "FAIL", name,
           g_wrong_any, reported, minutes);
    if (!pass) fails++;
    wwv_time_decoder_destroy(td);
}

static void m_burst(char *f, int k) {          /* minute 5: four field bits flipped */
    if (k != 5) return;
    static const int s[4] = {11, 21, 32, 52};
    for (int i = 0; i < 4; i++) f[s[i]] = f[s[i]] == '0' ? '1' : '0';
}
static void m_half_erased(char *f, int k) {
    for (int s = 1; s < 60; s++) if ((s * 7 + k * 13) % 2 == 0) f[s] = '.';
}
static void m_bad_bcd(char *f, int k) {        /* minute units 12 (bits 4 and 8) */
    (void)k; f[12] = '1'; f[13] = '1';
}
static void m_late_anchor(char *f, int k) {    /* every second labelled one too early */
    (void)k;
    char g[61];
    memcpy(g, f, 61);
    for (int s = 0; s < 60; s++) f[s] = g[(s + 1) % 60];
}

static void test_single_frame(void) {
    wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
    wwv_time_decoder_set_callback(td, on_result, NULL);
    reset_capture("one frame alone, with a bit error");
    g_truth = (utc_t){2026, 323, 12, 19};
    g_status = (status_t){-1, 0, 0, 0};
    char f[61];
    wwv_td_encode(2026, 323, 12, 19, -1, 0, 0, 0, f);
    f[10] = '0';                                /* minute units 9 -> 8 */
    feed(td, 5000.0f, f);
    /* The frame is complete and in range (hard_valid), just wrong: a lone frame
     * must not be CONFIRMED whatever it says, and this one must not be DECODED. */
    int pass = g_results == 1 && g_last.hard_valid && g_wrong_any == 0 &&
               g_last.tod_state != WWV_TD_CONFIRMED && g_last.date_state != WWV_TD_CONFIRMED;
    printf("%-4s %-58s tod %s, date %s\n", pass ? "ok" : "FAIL", g_name,
           wwv_td_state_name(g_last.tod_state), wwv_td_state_name(g_last.date_state));
    if (!pass) fails++;
    wwv_time_decoder_destroy(td);
}

static void test_bad_bcd_flagged(void) {
    wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
    wwv_time_decoder_set_callback(td, on_result, NULL);
    reset_capture("invalid BCD digit is not decodable alone");
    char f[61];
    wwv_td_encode(2026, 323, 12, 19, -1, 0, 0, 0, f);
    feed(td, 5000.0f, f);
    int clean_valid = g_last.hard_valid;
    f[12] = '1'; f[13] = '1';                   /* minute units 9 | 4 | 8 = 13 */
    feed(td, 65000.0f, f);
    int pass = clean_valid && !g_last.hard_valid;
    printf("%-4s %-58s clean frame %s, minute units 13 %s\n", pass ? "ok" : "FAIL", g_name,
           clean_valid ? "valid" : "INVALID", g_last.hard_valid ? "VALID" : "invalid");
    if (!pass) fails++;
    wwv_time_decoder_destroy(td);
}

static void monte_carlo(void) {
    static const double P01[2] = {0.033, 0.10}, P10[2] = {0.068, 0.20}, PP1[2] = {0.12, 0.36};
    int chains = 0, conf_wrong = 0, dec_wrong = 0, conf_tod = 0, results = 0;
    g_quiet = 1;
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 40; c++) {
        wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
        wwv_time_decoder_set_callback(td, on_result, NULL);
        reset_capture("random");
        /* random start; every fourth chain starts 10 minutes before a year end */
        int y = 2000 + (int)(urand() * 100);
        utc_t t0 = {y, 1 + (int)(urand() * ylen(y)), (int)(urand() * 24), (int)(urand() * 60)};
        if (c % 4 == 0) t0 = (utc_t){y, ylen(y), 23, 50};
        if (t0.year == 2099 && t0.doy > 300) t0.doy = 300;
        g_status = (status_t){(int)(urand() * 15) - 7, urand() < 0.5, 0, urand() < 0.1};
        g_status.dst2 = g_status.dst55;
        int minutes = 45;
        char f[61];
        for (int k = 0; k < minutes; k++) {
            g_truth = add_minutes(t0, k);
            if (g_truth.year > 2099) break;
            wwv_td_encode(g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute, g_status.dut1,
                          g_status.dst55, g_status.dst2, g_status.lsw, f);
            corrupt(f, P01[a], P10[b], PP1[b], 0.45, 0.08);
            feed(td, 5000.0f + 60000.0f * k, f);
        }
        chains++;
        results += g_results;
        conf_wrong += g_wrong_conf;
        dec_wrong += g_wrong_dec;
        conf_tod += g_last.tod_state == WWV_TD_CONFIRMED;
        wwv_time_decoder_destroy(td);
    }
    g_quiet = 0;
    int pass = conf_wrong == 0;
    printf("%-4s %-58s %d chains, %d minutes: CONFIRMED wrong %d, DECODED wrong %d, "
           "time of day CONFIRMED at the end %d/%d\n", pass ? "ok" : "FAIL",
           "random errors to 3x the worst air run never CONFIRMED wrong", chains, results, conf_wrong,
           dec_wrong, conf_tod, chains);
    if (!pass) fails++;
}

/*---------------------------------------------------------------- 4. WWVB */
static void wwvb_encode(utc_t t, char *f, int marker0) {
    for (int s = 0; s < 60; s++) f[s] = '0';
    f[60] = '\0';
    static const int P[7] = {0, 9, 19, 29, 39, 49, 59};
    for (int i = 0; i < 7; i++) f[P[i]] = 'P';
    if (!marker0) f[0] = '.';
#define PUT(sec, w, v) do { if ((v) & (w)) f[sec] = '1'; } while (0)
    int m10 = t.minute / 10, m1 = t.minute % 10, h10 = t.hour / 10, h1 = t.hour % 10;
    PUT(1, 4, m10); PUT(2, 2, m10); PUT(3, 1, m10);
    PUT(5, 8, m1); PUT(6, 4, m1); PUT(7, 2, m1); PUT(8, 1, m1);
    PUT(12, 2, h10); PUT(13, 1, h10);
    PUT(15, 8, h1); PUT(16, 4, h1); PUT(17, 2, h1); PUT(18, 1, h1);
    int d100 = t.doy / 100, d10 = (t.doy / 10) % 10, d1 = t.doy % 10;
    PUT(22, 2, d100); PUT(23, 1, d100);
    PUT(25, 8, d10); PUT(26, 4, d10); PUT(27, 2, d10); PUT(28, 1, d10);
    PUT(30, 8, d1); PUT(31, 4, d1); PUT(32, 2, d1); PUT(33, 1, d1);
    f[37] = '1';                                       /* UT1 negative */
    PUT(43, 1, 1);                                     /* 0.1 s */
    int y10 = (t.year % 100) / 10, y1 = t.year % 10;
    PUT(45, 8, y10); PUT(46, 4, y10); PUT(47, 2, y10); PUT(48, 1, y10);
    PUT(50, 8, y1); PUT(51, 4, y1); PUT(52, 2, y1); PUT(53, 1, y1);
#undef PUT
}

static void test_wwvb(int marker0) {
    wwv_time_decoder_t *td = wwv_time_decoder_create(NULL);
    wwv_time_decoder_set_callback(td, on_result, NULL);
    const char *name = marker0 ? "WWVB layout: not admitted, nothing reported"
                               : "WWVB layout without its second-0 marker";
    reset_capture(name);
    utc_t t0 = {2026, 323, 17, 42};
    int conf = 0, wwvb_tod = 0, reported = 0, admitted = 0;
    char f[61];
    g_quiet = 1;
    for (int k = 0; k < 30; k++) {
        utc_t t = add_minutes(t0, k);
        g_truth = t;
        wwvb_encode(t, f, marker0);
        feed(td, 5000.0f + 60000.0f * k, f);
        const wwv_td_result_t *r = &g_last;
        admitted += r->admitted;
        conf += r->tod_state == WWV_TD_CONFIRMED || r->date_state == WWV_TD_CONFIRMED ||
                r->dut1_state == WWV_TD_CONFIRMED || r->dst_state == WWV_TD_CONFIRMED ||
                r->lsw_state == WWV_TD_CONFIRMED;
        reported += r->tod_state != WWV_TD_NONE || r->date_state != WWV_TD_NONE;
        wwvb_tod += r->tod_state != WWV_TD_NONE && r->hour == t.hour && r->minute == t.minute;
    }
    g_quiet = 0;
    int pass = conf == 0 && wwvb_tod == 0 && (!marker0 || (admitted == 0 && reported == 0));
    printf("%-4s %-58s admitted %d/30, CONFIRMED %d, time reported %d, WWVB time %d\n",
           pass ? "ok" : "FAIL", name, admitted, conf, reported, wwvb_tod);
    if (!pass) fails++;
    wwv_time_decoder_destroy(td);
}

int main(void) {
    test_encoder();
    status_t nov = {-1, 0, 0, 0};
    clean_run("2026-323 12:10 on, as on air", (utc_t){2026, 323, 12, 10}, 10, nov);
    clean_run("day-of-year rollover 323 -> 324", (utc_t){2026, 323, 23, 55}, 16, nov);
    clean_run("year boundary 2026-365 -> 2027-001", (utc_t){2026, 365, 23, 56}, 12, nov);
    clean_run("leap year end 2028-366 -> 2029-001", (utc_t){2028, 366, 23, 57}, 12, nov);
    clean_run("DST change day (55 = 1, 2 = 0), 2026-067", (utc_t){2026, 67, 15, 0}, 10,
              (status_t){-1, 1, 0, 0});
    clean_run("DST in effect (55 = 2 = 1), 2026-190", (utc_t){2026, 190, 6, 30}, 10,
              (status_t){2, 1, 1, 0});
    clean_run("leap second warning, UT1 +0.7, 2026-181", (utc_t){2026, 181, 20, 0}, 10,
              (status_t){7, 1, 1, 1});
    clean_run("UT1 -0.7, 2099-300", (utc_t){2099, 300, 0, 0}, 10, (status_t){-7, 0, 0, 0});
    test_single_frame();
    test_bad_bcd_flagged();
    corrupted_run("burst of four bit errors in minute 5 of 12", (utc_t){2026, 323, 12, 10}, 12, m_burst, 0);
    corrupted_run("half the seconds erased", (utc_t){2026, 323, 12, 10}, 15, m_half_erased, 0);
    corrupted_run("minute units 12-15 in every frame", (utc_t){2026, 323, 12, 10}, 12, m_bad_bcd, 0);
    corrupted_run("anchor 1 s late: wrong second phase", (utc_t){2026, 323, 12, 10}, 15, m_late_anchor, 1);
    monte_carlo();
    test_wwvb(1);
    test_wwvb(0);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
