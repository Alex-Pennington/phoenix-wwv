/* Tick detector marker candidates (tick_detector_set_candidate_callback).
 *
 * A synthetic 1000 Hz signal at the sync-channel input: a 5 ms tick every second
 * except :29 and :59, an 800 ms minute marker at :00. Before the :00 of minutes
 * 1, 3 and 5 a stray 20 ms burst at :59.84 (as the noise runs before :00 on air)
 * puts the detector in its 500 ms cooldown through the marker's start, so the run
 * starts about +375 ms late and ends on time. The added noise is negligible
 * (sigma 1e-4): from 1e-3 up, noise runs dropped by the detector take the same
 * cooldown at random (the live mechanism) and can hide the stray itself, which
 * makes the stray minutes a matter of the seed.
 *
 * 1. Stray minutes (1, 3, 5): no TMARK, and a candidate whose end reading is
 *    within 20 ms of :00 and whose start reading is off by more than the sync
 *    detector's 150 ms tolerance.
 * 2. Every minute gives exactly one of a TMARK within 20 ms or such a
 *    candidate, and nothing is reported off :00.
 */
#include <stdio.h>
#include <math.h>
#include "tick_detector.h"
#include "channel_filters.h"

#define FS 50000.0
#define FIRST_MS 20000.0   /* first :00 */
#define MINUTES 6

static int n_tmark, n_cand;
static float tmark_lead[32], cand_ls[32], cand_le[32];
static void on_tmark(const tick_marker_event_t *e, void *u) {
    (void)u;
    if (n_tmark < 32) tmark_lead[n_tmark] = e->start_timestamp_ms;
    n_tmark++;
}
static void on_cand(const tick_marker_candidate_t *c, void *u) {
    (void)u;
    if (n_cand < 32) { cand_ls[n_cand] = c->lead_from_start_ms; cand_le[n_cand] = c->lead_from_end_ms; }
    n_cand++;
}

static unsigned nr = 4321;
static double gauss(void) {
    double s = 0;
    for (int i = 0; i < 12; i++) { nr = nr * 1103515245u + 12345u; s += (nr >> 8) / 16777216.0; }
    return s - 6.0;
}

/* Offset of t from the nearest :00 */
static double off00(double t) {
    double k = floor((t - FIRST_MS) / 60000.0 + 0.5);
    return t - FIRST_MS - 60000.0 * k;
}

int main(void) {
    int fails = 0;
    tick_detector_t *td = tick_detector_create(NULL);
    tick_detector_set_marker_callback(td, on_tmark, NULL);
    tick_detector_set_candidate_callback(td, on_cand, NULL);
    sync_channel_t si, sq;
    sync_channel_init(&si); sync_channel_init(&sq);
    const double seconds = FIRST_MS / 1000.0 + 60.0 * MINUTES - 5.0;
    for (long n = 0; n < (long)(seconds * FS); n++) {
        double t = n / FS, tms = t * 1000.0;
        double rel = tms - FIRST_MS + 60000.0 * 10;        /* keep it positive */
        double k = floor(rel / 60000.0), ph = rel - 60000.0 * k;   /* ms into the minute */
        int minute = (int)k - 10, sec = (int)(ph / 1000.0);
        double in_sec = ph - 1000.0 * sec;
        double a = 0.0;
        if (ph < 800.0) a = 1.0;                                    /* minute marker */
        else if (sec != 29 && sec != 59 && in_sec < 5.0) a = 1.0;   /* tick */
        /* stray 20 ms burst at :59.84 before the :00 of minutes 1, 3, 5 */
        if (minute >= 0 && (minute % 2) == 0 && sec == 59 && in_sec >= 840.0 && in_sec < 860.0) a = 1.0;
        double I = a * cos(2 * M_PI * 1000 * t) + 1e-4 * gauss();
        double Q = a * sin(2 * M_PI * 1000 * t) + 1e-4 * gauss();
        tick_detector_process_sample(td, sync_channel_process(&si, (float)I), sync_channel_process(&sq, (float)Q));
    }
    tick_detector_destroy(td);

    /* Per minute m (:00 at FIRST_MS + 60k m): a TMARK within 20 ms, a candidate
     * whose end reading is within 20 ms (start reading off by more than the sync
     * detector's 150 ms), or neither. */
    int got_tmark[MINUTES] = {0}, got_cand[MINUTES] = {0}, bad = 0;
    for (int i = 0; i < n_tmark && i < 32; i++) {
        double o = off00(tmark_lead[i]);
        int m = (int)floor((tmark_lead[i] - FIRST_MS) / 60000.0 + 0.5);
        printf("     TMARK minute %d lead %+.1f ms from :00\n", m, o);
        if (m >= 0 && m < MINUTES && fabs(o) <= 20.0) got_tmark[m] = 1; else bad++;
    }
    for (int i = 0; i < n_cand && i < 32; i++) {
        double oe = off00(cand_le[i]), os = off00(cand_ls[i]);
        int m = (int)floor((cand_le[i] - FIRST_MS) / 60000.0 + 0.5);
        printf("     candidate minute %d: end reading %+.1f ms, start reading %+.1f ms from :00\n", m, oe, os);
        if (m >= 0 && m < MINUTES && fabs(oe) <= 20.0 && fabs(os) > 150.0) got_cand[m] = 1; else bad++;
    }
    int stray_ok = 0, covered = 0;
    for (int m = 0; m < MINUTES; m++) {
        if ((m % 2) == 1 && got_cand[m] && !got_tmark[m]) stray_ok++;
        if (got_cand[m] + got_tmark[m] == 1) covered++;
    }
    printf("%-4s stray before :00: %d of %d minutes give a candidate on the end reading and no TMARK\n",
           stray_ok == MINUTES / 2 ? "ok" : "FAIL", stray_ok, MINUTES / 2);
    if (stray_ok != MINUTES / 2) fails++;
    printf("%-4s every minute: %d of %d give exactly one of TMARK or candidate, %d off :00\n",
           (covered == MINUTES && bad == 0) ? "ok" : "FAIL", covered, MINUTES, bad);
    if (covered != MINUTES || bad != 0) fails++;

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
