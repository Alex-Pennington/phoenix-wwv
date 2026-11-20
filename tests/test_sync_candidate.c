/* Sync detector marker-candidate tests (sync_detector_marker_candidate).
 *
 * A candidate is a run a detector rejected as a marker that may be one. It may
 * confirm a minute phase sync already holds, never start or move one.
 *
 * 1. Weak minutes: one tick-path marker, then two minutes whose marker shows only
 *    as a candidate (start hidden, end on time). Sync must lock on the third
 *    minute, on the held phase; without the candidates it stays TENTATIVE.
 * 2. No phase: candidates alone, in ACQUIRING, start nothing.
 * 3. Off the phase: the :52.5 voice (a candidate 7.5 s before :00), and one
 *    200 ms off :00, after one marker. Neither may count or move the anchor.
 * 4. A false first marker at :20, then candidates at the real :00 for 5 minutes.
 *    Sync must not lock, and the anchor must stay where the markers put it.
 * 5. A candidate, then the marker detector's own event for the same minute: one
 *    confirmation, the anchor taken from the marker.
 * 6. Holdover expired (RECOVERING): a candidate on the retained phase re-locks.
 */
#include <stdio.h>
#include <math.h>
#include "sync_detector.h"

static int fails;
static void check(int ok, const char *what) {
    printf("%-4s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

/* A tick-path marker with its leading edge at t, then 3.2 s of periodic checks
 * so it is confirmed by the timeout, as in the chain. */
static void marker(sync_detector_t *sd, float t) {
    sync_detector_tick_marker(sd, t, 810.0f, 100.0f);
    for (float c = t + 50.0f; c <= t + 3200.0f; c += 50.0f) sync_detector_periodic_check(sd, c);
}

/* A tick-path candidate for a marker at t whose start the cooldown hid: the run
 * started 350 ms late, so the start reading is t + 350 and the end reading is t
 * (plus err ms of measurement). */
static void candidate(sync_detector_t *sd, float t, float err) {
    sync_detector_marker_candidate(sd, t + 350.0f, t + err, "CANDT");
    sync_detector_periodic_check(sd, t + 900.0f);
}

/* Periodic checks every 50 ms over [from, to) */
static void idle(sync_detector_t *sd, float from, float to) {
    for (float c = from; c < to; c += 50.0f) sync_detector_periodic_check(sd, c);
}

int main(void) {
    /* 1. Marker at 0.4 s; candidates at 60.4 and 120.4 s, 30 ms early/late. */
    sync_detector_t *sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    idle(sd, 3650.0f, 60400.0f);
    candidate(sd, 60400.0f, -30.0f);
    idle(sd, 61350.0f, 120400.0f);
    candidate(sd, 120400.0f, 30.0f);
    check(sync_detector_get_state(sd) == SYNC_LOCKED, "weak minutes: LOCKED on the third minute");
    check(fabsf(sync_detector_get_last_marker_ms(sd) - 120400.0f) < 0.5f,
          "weak minutes: confirmed on the held phase (120400.0), not the candidate's time");
    check(sync_detector_get_confirmed_count(sd) == 3, "weak minutes: three minutes confirmed");
    sync_detector_destroy(sd);

    sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    idle(sd, 3650.0f, 125000.0f);
    check(sync_detector_get_state(sd) == SYNC_TENTATIVE, "weak minutes: without the candidates, TENTATIVE");
    sync_detector_destroy(sd);

    /* 2. Candidates every minute from 0.4 s, nothing else. */
    sd = sync_detector_create(NULL);
    for (int m = 0; m < 5; m++) {
        candidate(sd, 400.0f + 60000.0f * m, 0.0f);
        idle(sd, 1350.0f + 60000.0f * m, 60400.0f + 60000.0f * m);
    }
    check(sync_detector_get_state(sd) == SYNC_ACQUIRING && sync_detector_get_confirmed_count(sd) == 0,
          "no phase: candidates alone leave sync ACQUIRING");
    sync_detector_destroy(sd);

    /* 3. Marker at 0.4 s; voice candidates at 52.9 + 60k (both readings off);
     *    one candidate 200 ms late at 120.6 s. */
    sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    for (int m = 0; m < 4; m++) {
        float v = 52900.0f + 60000.0f * m;
        idle(sd, (m ? 61350.0f + 60000.0f * (m - 1) : 3650.0f), v);
        sync_detector_marker_candidate(sd, v, v + 2500.0f, "CANDT");   /* a 4 s voice run */
        idle(sd, v, v + 8450.0f);
        if (m == 1) candidate(sd, 120400.0f, 200.0f);
    }
    check(sync_detector_get_state(sd) == SYNC_TENTATIVE && sync_detector_get_confirmed_count(sd) == 1,
          "off the phase: voice at :52.5 and a candidate 200 ms off do not count");
    check(fabsf(sync_detector_get_last_marker_ms(sd) - 400.0f) < 0.5f, "off the phase: anchor unmoved");
    sync_detector_destroy(sd);

    /* 4. False marker at 20.4 s; real minute at 60.4 + 60k shows only as candidates. */
    sd = sync_detector_create(NULL);
    marker(sd, 20400.0f);
    int locked = 0;
    for (int m = 1; m <= 5; m++) {
        idle(sd, (m == 1 ? 23650.0f : 1350.0f + 60000.0f * (m - 1)), 60400.0f * 1.0f + 60000.0f * (m - 1));
        candidate(sd, 60400.0f + 60000.0f * (m - 1), 0.0f);
        if (sync_detector_get_state(sd) == SYNC_LOCKED) locked = 1;
    }
    check(!locked, "false first marker: candidates off its phase never lock it");
    check(fabsf(sync_detector_get_last_marker_ms(sd) - 20400.0f) < 0.5f,
          "false first marker: candidates neither move nor confirm the anchor");
    sync_detector_destroy(sd);

    /* 5. Marker at 0.4 s; at 60.4 s a candidate, then the marker detector's
     *    event (lead 60.47 s) for the same minute. */
    sd = sync_detector_create(NULL);
    marker(sd, 400.0f);
    idle(sd, 3650.0f, 60400.0f);
    candidate(sd, 60400.0f, 10.0f);
    sync_detector_marker_event(sd, 60470.0f, 1.0f, 790.0f);
    idle(sd, 61400.0f, 65000.0f);
    check(sync_detector_get_confirmed_count(sd) == 2, "same minute: counted once (2 confirmations)");
    check(fabsf(sync_detector_get_last_marker_ms(sd) - 60470.0f) < 0.5f,
          "same minute: anchor taken from the marker");
    sync_detector_destroy(sd);

    /* 6. Lock at 0.4/60.4/120.4 s, then 22 minutes of nothing, then a candidate
     *    on the phase at 23 * 60 s + 120.4 s. */
    sd = sync_detector_create(NULL);
    for (int m = 0; m < 3; m++) {
        marker(sd, 400.0f + 60000.0f * m);
        idle(sd, 3650.0f + 60000.0f * m, 60400.0f + 60000.0f * m);
    }
    check(sync_detector_get_state(sd) == SYNC_LOCKED, "recovery: LOCKED on three markers");
    float t = 120400.0f + 23.0f * 60000.0f;
    idle(sd, 180400.0f, t);
    check(sync_detector_get_state(sd) == SYNC_RECOVERING, "recovery: RECOVERING after the holdover");
    candidate(sd, t, 20.0f);
    idle(sd, t + 950.0f, t + 2000.0f);
    check(sync_detector_get_state(sd) == SYNC_LOCKED, "recovery: a candidate on the retained phase re-locks");
    sync_detector_destroy(sd);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
