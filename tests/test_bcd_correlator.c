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
 */
#include <stdio.h>
#include <string.h>
#include "sync_detector.h"
#include "bcd_correlator.h"

static int fails;
static char got[60];

static void on_sym(const bcd_symbol_event_t *e, void *u) {
    (void)u;
    if (e->second >= 0 && e->second < 60) got[e->second] = (char)('0' + e->symbol);
}

static char truth(int s) {
    if (s % 10 == 9) return '2';                 /* P */
    return (s * 7 % 3 == 0) ? '1' : '0';         /* a fixed 0/1 pattern */
}

static float width(char t, int s) {
    float jitter = (float)((s * 37) % 21) - 10.0f;  /* +-10 ms */
    return (t == '2' ? 800.0f : t == '1' ? 500.0f : 200.0f) + jitter;
}

static void run(const char *name, int blips, float start_ms) {
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
    for (int s = 1; s <= 59; s++) {
        float t0 = a + 1000.0f * s + start_ms;
        bcd_correlator_time_event(c, t0, width(truth(s), s), 1.0f);
        if (blips && s % 3 == 1)
            bcd_correlator_time_event(c, t0 + 530.0f, 120.0f, 1.0f);
    }
    int ok = 0, n = 0;
    for (int s = 1; s <= 58; s++) {       /* window 59 is still open */
        n++;
        if (got[s] == truth(s)) ok++;
        else printf("     %s: second %d got %c want %c\n", name, s, got[s] ? got[s] : '-', truth(s));
    }
    int pass = ok == n;
    printf("%-4s %-50s %d/%d symbols right\n", pass ? "ok" : "FAIL", name, ok, n);
    if (!pass) fails++;
    bcd_correlator_destroy(c);
    sync_detector_destroy(sd);
}

int main(void) {
    run("clean minute", 0, 20.0f);
    run("mid-second detection in every third second", 1, 20.0f);
    run("pulses start 40 ms before the anchor second", 0, -40.0f);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
