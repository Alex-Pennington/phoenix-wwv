/* Sync detector minute-anchor tests, driven with tick-path marker events.
 *
 * 1. A false first marker followed by real ones: sync must re-anchor on the real
 *    minute once two real markers one minute apart outvote it.
 * 2. A lone false marker mid-stream after real lock: the anchor must never move
 *    to it, and the next real marker must be accepted.
 * 3. Two anchors interleaved (real every 60 s, false every 60 s, offset 25 s):
 *    the anchor must stay on the real chain and never flip.
 * 4. A lone marker with only the 50 ms periodic check after it (a minute where
 *    one detector fires): it must anchor within PENDING_TIMEOUT_MS (3 s), not
 *    wait for the next marker event a minute later.
 */
#include <stdio.h>
#include <math.h>
#include "sync_detector.h"

static int fails;
static void expect(const char *what, float got, float want) {
    int ok = fabsf(got - want) < 1.0f;
    printf("%-4s %-58s anchor %.1f  want %.1f\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) fails++;
}

/* Feed one tick-path marker. A lone pending marker is confirmed (or rejected)
 * by the next event's timeout check, so each expectation follows the marker
 * after the one it is about. */
static void marker(sync_detector_t *sd, float t) {
    sync_detector_tick_marker(sd, t, 810.0f, 100.0f);
    sync_detector_periodic_check(sd, t + 50.0f);
}

int main(void) {
    /* 1. False first marker at 6.4 s, real minutes at 46.4 + 60k s. */
    sync_detector_t *sd = sync_detector_create(NULL);
    marker(sd, 6400.0f);
    marker(sd, 46400.0f);
    marker(sd, 106400.0f);
    marker(sd, 166400.0f);
    expect("false first: re-anchors on the second real marker",
           sync_detector_get_last_marker_ms(sd), 106400.0f);
    marker(sd, 226400.0f);
    marker(sd, 230000.0f);  /* flush the last pending marker */
    expect("false first: follows the real minute afterwards",
           sync_detector_get_last_marker_ms(sd), 226400.0f);
    sync_detector_destroy(sd);

    /* 2. Real lock at 0.4 + 60k s, lone false marker at 210 s. */
    sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    marker(sd, 60400.0f);
    marker(sd, 120400.0f);
    marker(sd, 180400.0f);
    marker(sd, 210000.0f);  /* lone false marker, 29.6 s after a real one */
    marker(sd, 240400.0f);
    expect("lone false: anchor stays on the real minute",
           sync_detector_get_last_marker_ms(sd), 180400.0f);
    marker(sd, 300400.0f);
    marker(sd, 304000.0f);  /* flush */
    expect("lone false: next real markers accepted",
           sync_detector_get_last_marker_ms(sd), 300400.0f);
    sync_detector_destroy(sd);

    /* 3. Interleaved: real at 0.4 + 60k, false at 25.4 + 60k. */
    sd = sync_detector_create(NULL);
    int flips = 0;
    for (int m = 0; m < 6; m++) {
        marker(sd, 400.0f + 60000.0f * m);
        marker(sd, 25400.0f + 60000.0f * m);   /* confirms or rejects the real one */
        float a = sync_detector_get_last_marker_ms(sd);
        if (fmodf(a - 400.0f, 60000.0f) > 1.0f) flips++;
    }
    expect("interleaved: anchor on the real chain",
           sync_detector_get_last_marker_ms(sd), 300400.0f);   /* real m=5 */
    printf("%-4s interleaved: anchor never on the false chain (%d flips)\n", flips ? "FAIL" : "ok", flips);
    if (flips) fails++;
    sync_detector_destroy(sd);

    /* 4. Real lock at 0.4 + 60k s, then a lone marker at 180.4 s and periodic
     *    checks only, as the harness and waterfall.c call them. */
    sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    marker(sd, 60400.0f);
    marker(sd, 120400.0f);
    sync_detector_tick_marker(sd, 180400.0f, 810.0f, 100.0f);
    for (float t = 180450.0f; t <= 183600.0f; t += 50.0f) sync_detector_periodic_check(sd, t);
    expect("lone marker: anchored by the periodic check within 3.2 s",
           sync_detector_get_last_marker_ms(sd), 180400.0f);
    sync_detector_destroy(sd);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
