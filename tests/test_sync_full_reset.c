/* Sync detector full reset: the anchor does not survive it.
 *
 * Sync locks on markers at 0.4 + 60k s for minutes 0..4, then the markers stop.
 * After the 20-minute holdover it is RECOVERING, and 60 minutes after the loss
 * the retention window ends in a full reset (ACQUIRING). Two hours after the
 * reset the signal is back, on a phase `shift` ms from the old one. The markers
 * of the new phase must be taken exactly as a fresh detector takes them: the
 * first is confirmed as a first marker, the same marker brings LOCKED, the held
 * anchor is the true one, and no phase break, phase reset or epoch is counted.
 *
 * Before the fix the old anchor survived the reset. At +2.3 s the first new
 * marker passed the +-5 s interval test against it and the move was counted as
 * a phase break and a phase reset (which drops the decoder's chain). At +20 s
 * the first new marker was rejected, and the second re-anchored, counting the
 * same break. At 0 the old phase is right and both must agree. */
#include <stdio.h>
#include <math.h>
#include "sync_detector.h"

#define STEP_MS        100.0
#define LOCK_MINUTES   5
#define GAP_AFTER_MS   (2.0 * 3600.0 * 1000.0)   /* reacquire this long after the reset */
#define NEW_MINUTES    6

static int fails;
static void check(int ok, const char *what, double got, double want) {
    printf("%-4s %-66s %.1f  want %.1f\n", ok ? "ok" : "FAIL", what, got, want);
    if (!ok) fails++;
}

static double g_now, g_reset_ms, g_lock_ms;
static int g_reset_seen;
static void on_state(sync_state_t o, sync_state_t n, float c, void *u) {
    (void)c; (void)u;
    if (n == SYNC_ACQUIRING && o == SYNC_RECOVERING && !g_reset_seen) { g_reset_seen = 1; g_reset_ms = g_now; }
    if (n == SYNC_LOCKED && g_reset_seen && g_lock_ms < 0) g_lock_ms = g_now;
}

/* One tick-path marker reported 800 ms after its leading edge, then periodic
 * checks every STEP_MS from report time to end_ms */
static void run_to(sync_detector_t *sd, double *now, double end_ms) {
    for (; *now <= end_ms; *now += STEP_MS) { g_now = *now; sync_detector_periodic_check(sd, *now); }
}
static void marker(sync_detector_t *sd, double *now, double lead_ms) {
    run_to(sd, now, lead_ms + 800.0);
    g_now = *now;
    sync_detector_tick_marker(sd, lead_ms, 800.0f, 1.0f);
}

typedef struct {
    double first_anchor;    /* anchor after the first new marker's timeout */
    int first_count;        /* confirmed markers it added */
    int lock_marker;        /* new marker after which LOCKED, -1 = never */
    double anchor;          /* at the end */
    int breaks, resets, epoch;
    sync_state_t state;
} result_t;

/* The new phase's markers at t0 + 60k s, on a detector given as it is */
static result_t reacquire(sync_detector_t *sd, double *now, double t0) {
    result_t r = { .lock_marker = -1 };
    sync_phase_stats_t ps0 = sync_detector_get_phase_stats(sd);
    int e0 = sync_detector_get_phase_epoch(sd), r0 = sync_detector_get_phase_resets(sd);
    int c0 = sync_detector_get_confirmed_count(sd);
    for (int k = 0; k < NEW_MINUTES; k++) {
        marker(sd, now, t0 + 60000.0 * k);
        run_to(sd, now, t0 + 60000.0 * k + 800.0 + 3500.0);   /* past PENDING_TIMEOUT_MS */
        if (k == 0) {
            r.first_anchor = sync_detector_get_last_marker_ms(sd);
            r.first_count = sync_detector_get_confirmed_count(sd) - c0;
        }
        if (r.lock_marker < 0 && sync_detector_get_state(sd) == SYNC_LOCKED) r.lock_marker = k;
    }
    sync_phase_stats_t ps = sync_detector_get_phase_stats(sd);
    r.anchor = sync_detector_get_last_marker_ms(sd);
    r.breaks = ps.breaks - ps0.breaks;
    r.resets = sync_detector_get_phase_resets(sd) - r0;
    r.epoch = sync_detector_get_phase_epoch(sd) - e0;
    r.state = sync_detector_get_state(sd);
    return r;
}

static void scenario(double shift_ms) {
    char what[96];
    printf("--- new phase %+.0f ms from the old one\n", shift_ms);

    sync_detector_t *sd = sync_detector_create(NULL);
    sync_detector_set_state_callback(sd, on_state, NULL);
    g_reset_seen = 0; g_reset_ms = 0; g_lock_ms = -1;
    double now = 0.0;
    for (int k = 0; k < LOCK_MINUTES; k++) marker(sd, &now, 400.0 + 60000.0 * k);
    run_to(sd, &now, 400.0 + 60000.0 * (LOCK_MINUTES - 1) + 5000.0);
    check(sync_detector_get_state(sd) == SYNC_LOCKED, "locked before the loss (state)",
          sync_detector_get_state(sd), SYNC_LOCKED);

    /* silence until the reset */
    double last_ms = 400.0 + 60000.0 * (LOCK_MINUTES - 1);
    while (!g_reset_seen && now < last_ms + 3.0 * 3600.0 * 1000.0) run_to(sd, &now, now + 60000.0);
    check(g_reset_seen, "full reset after holdover and retention (seen)", g_reset_seen, 1);
    snprintf(what, sizeof what, "reset %.1f min after the last marker (at least 80)", (g_reset_ms - last_ms) / 60000.0);
    check(g_reset_ms - last_ms >= 80.0 * 60000.0, what, (g_reset_ms - last_ms) / 60000.0, 80.0);
    check(sync_detector_get_last_marker_ms(sd) == 0.0, "anchor cleared by the reset",
          sync_detector_get_last_marker_ms(sd), 0.0);

    /* the new phase, 2 h after the reset: the first marker at a whole minute of
     * the old phase plus shift_ms */
    double n = ceil((g_reset_ms + GAP_AFTER_MS - 400.0) / 60000.0);
    double t0 = 400.0 + 60000.0 * n + shift_ms;
    result_t got = reacquire(sd, &now, t0);

    /* a fresh detector on the same new markers, from the same stream time */
    sync_detector_t *fresh = sync_detector_create(NULL);
    double fnow = t0 - 10000.0;
    result_t want = reacquire(fresh, &fnow, t0);

    check(got.first_count == 1 && got.first_anchor == t0, "first new marker confirmed as the anchor (count)",
          got.first_count, 1);
    check(got.first_anchor == t0, "first new marker confirmed as the anchor (anchor - t0, ms)",
          got.first_anchor - t0, 0.0);
    check(got.lock_marker == want.lock_marker && got.lock_marker >= 0,
          "LOCKED on the same new marker as a fresh detector (index)", got.lock_marker, want.lock_marker);
    check(got.state == SYNC_LOCKED, "LOCKED at the end (state)", got.state, SYNC_LOCKED);
    check(got.anchor == t0 + 60000.0 * (NEW_MINUTES - 1), "anchor on the last new marker (anchor - want, ms)",
          got.anchor - (t0 + 60000.0 * (NEW_MINUTES - 1)), 0.0);
    check(got.breaks == 0, "no phase break counted", got.breaks, 0);
    check(got.resets == 0, "no phase reset counted", got.resets, 0);
    check(got.epoch == 0, "no new phase epoch", got.epoch, 0);
    check(got.breaks == want.breaks && got.resets == want.resets && got.epoch == want.epoch,
          "break, reset and epoch counts as a fresh detector's (breaks)", got.breaks, want.breaks);
    sync_detector_destroy(fresh);
    sync_detector_destroy(sd);
}

int main(void) {
    scenario(0.0);
    scenario(2300.0);
    scenario(20000.0);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails != 0;
}
