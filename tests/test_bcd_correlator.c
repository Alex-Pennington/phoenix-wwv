/* BCD correlator window tests, driven with BTIME events against a locked sync anchor.
 *
 * 1. Clean minute: one pulse per second, starting 20 ms after the second, with the
 *    WWV widths (0 = 200 ms, 1 = 500 ms, P = 800 ms). Every window gives its symbol.
 * 2. Same minute plus a short mid-second detection (+550 ms, 120 ms wide) in every
 *    third second (s % 3 == 1, which holds 0s and P markers), as the air captures
 *    show. A window's symbol is the width of the pulse at the second's start; a
 *    later event in the window does not change it.
 * 3. Clean minute, every pulse detected 40 ms BEFORE its anchor second (a late anchor
 *    or early start jitter). Each pulse must still land in its own second's window.
 * 4. Clean minute sent one minute after the last marker, with no new marker (a stale
 *    anchor). Symbols must be right and timestamped in that minute, not 60 s early.
 * Every case also checks each symbol's timestamp lies inside its own second.
 * 5. The next minute's marker is confirmed during second 1, after that second's
 *    pulse opened its window under the old anchor (index 61) and before a later
 *    mid-second event. Second 1 must be emitted once, from its boundary pulse.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sync_detector.h"
#include "bcd_correlator.h"

static int fails;
static char got[60];
static float stamp[60];

static void on_sym(const bcd_symbol_event_t *e, void *u) {
    (void)u;
    if (e->second >= 0 && e->second < 60) {
        got[e->second] = (char)('0' + e->symbol);
        stamp[e->second] = e->timestamp_ms;
    }
}

static char truth(int s) {
    if (s % 10 == 9) return '2';                 /* P */
    return (s * 7 % 3 == 0) ? '1' : '0';         /* a fixed 0/1 pattern */
}

static float width(char t, int s) {
    float jitter = (float)((s * 37) % 21) - 10.0f;  /* +-10 ms */
    return (t == '2' ? 800.0f : t == '1' ? 500.0f : 200.0f) + jitter;
}

static void run(const char *name, int blips, float start_ms, int minute) {
    sync_detector_t *sd = sync_detector_create(NULL);
    for (int m = 0; m < 4; m++) {
        sync_detector_tick_marker(sd, 400.0f + 60000.0f * m, 810.0f, 100.0f);
        sync_detector_periodic_check(sd, 450.0f + 60000.0f * m);
    }
    if (sync_detector_get_state(sd) != SYNC_LOCKED) {
        printf("FAIL %s: sync not LOCKED after four markers\n", name);
        fails++;
        sync_detector_destroy(sd);
        return;
    }
    float a = sync_detector_get_last_marker_ms(sd);
    bcd_correlator_t *c = bcd_correlator_create(NULL);
    bcd_correlator_set_sync_source(c, sd);
    bcd_correlator_set_callback(c, on_sym, NULL);
    memset(got, 0, sizeof got);
    memset(stamp, 0, sizeof stamp);
    for (int s = 1; s <= 59; s++) {
        float t0 = a + 60000.0f * minute + 1000.0f * s + start_ms;
        bcd_correlator_time_event(c, t0, width(truth(s), s), 1.0f);
        if (blips && s % 3 == 1)
            bcd_correlator_time_event(c, t0 + 530.0f, 120.0f, 1.0f);
    }
    int ok = 0, n = 0;
    for (int s = 1; s <= 58; s++) {       /* window 59 is still open */
        n++;
        float into = stamp[s] - (a + 60000.0f * minute + 1000.0f * s);
        if (got[s] == truth(s) && into >= 0.0f && into < 1000.0f) ok++;
        else printf("     %s: second %d got %c want %c, stamp %+.0f ms into the second\n",
                    name, s, got[s] ? got[s] : '-', truth(s), into);
    }
    int pass = ok == n;
    printf("%-4s %-50s %d/%d symbols right\n", pass ? "ok" : "FAIL", name, ok, n);
    if (!pass) fails++;
    bcd_correlator_destroy(c);
    sync_detector_destroy(sd);
}

static int sec_count[60];
static char sec_sym[60];
static void on_sym_count(const bcd_symbol_event_t *e, void *u) {
    float minute_start = *(const float *)u;
    if (e->timestamp_ms < minute_start || e->second < 0 || e->second >= 60) return;
    sec_count[e->second]++;
    sec_sym[e->second] = (char)('0' + e->symbol);
}

static void run_reanchor(void) {
    const char *name = "next marker confirmed during second 1";
    sync_detector_t *sd = sync_detector_create(NULL);
    for (int m = 0; m < 4; m++) {
        sync_detector_tick_marker(sd, 400.0f + 60000.0f * m, 810.0f, 100.0f);
        sync_detector_periodic_check(sd, 450.0f + 60000.0f * m);
    }
    sync_detector_periodic_check(sd, 180400.0f + 3100.0f);   /* confirm the fourth */
    float a = sync_detector_get_last_marker_ms(sd);
    float next = a + 60000.0f;
    bcd_correlator_t *c = bcd_correlator_create(NULL);
    bcd_correlator_set_sync_source(c, sd);
    bcd_correlator_set_callback(c, on_sym_count, &next);
    memset(sec_count, 0, sizeof sec_count);
    memset(sec_sym, 0, sizeof sec_sym);
    for (int s = 1; s <= 59; s++)
        bcd_correlator_time_event(c, a + 1000.0f * s + 20.0f, width(truth(s), s), 1.0f);
    /* second 1 of the next minute: its pulse (a 0) arrives under the old anchor */
    bcd_correlator_time_event(c, next + 1020.0f, 200.0f, 1.0f);
    /* both detectors report the next minute's marker; the pair confirms it now */
    sync_detector_tick_marker(sd, next + 3.0f, 810.0f, 100.0f);
    sync_detector_marker_event(sd, next + 20.0f, 5.0f, 800.0f);
    sync_detector_periodic_check(sd, next + 1300.0f);
    float b = sync_detector_get_last_marker_ms(sd);
    /* a mid-second event in second 1, then second 2's pulse */
    bcd_correlator_time_event(c, next + 1550.0f, 500.0f, 1.0f);
    bcd_correlator_time_event(c, next + 2020.0f, 200.0f, 1.0f);
    bcd_correlator_time_event(c, next + 3020.0f, 200.0f, 1.0f);
    int pass = fabsf(b - (next + 3.0f)) < 1.0f && sec_count[1] == 1 && sec_sym[1] == '0'
               && sec_count[2] == 1;
    printf("%-4s %-50s anchor %+.0f ms, second 1 emitted %d time(s) as %c, second 2 %d\n",
           pass ? "ok" : "FAIL", name, b - next, sec_count[1], sec_sym[1] ? sec_sym[1] : '-',
           sec_count[2]);
    if (!pass) fails++;
    bcd_correlator_destroy(c);
    sync_detector_destroy(sd);
}

int main(void) {
    run("clean minute", 0, 20.0f, 0);
    run("mid-second detection in every third second", 1, 20.0f, 0);
    run("pulses start 40 ms before the anchor second", 0, -40.0f, 0);
    run("minute after the last marker (stale anchor)", 0, 20.0f, 1);
    run_reanchor();
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
