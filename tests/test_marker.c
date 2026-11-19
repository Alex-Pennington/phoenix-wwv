/* Marker detector tests on a synthetic 1000 Hz signal at the sync-channel output.
 *
 * 1. Edges: 800 ms markers at a known time. The leading edge must land within
 *    +-30 ms of truth and the length estimate within 800 +-100 ms; the raw window
 *    run is 1.6-1.8 s.
 * 2. Timeout reset: the in-band level steps up 4x and stays there (a tone, a
 *    fade-up). The detector times out once; without the baseline reset it keeps
 *    timing out and misses every later marker.
 */
#include <stdio.h>
#include <math.h>
#include "marker_detector.h"

#define FS 50000.0

static int n_ev; static float ev_start[32], ev_dur[32];
static void on_marker(const marker_event_t *e, void *u) {
    (void)u;
    if (n_ev < 32) { ev_start[n_ev] = e->start_timestamp_ms; ev_dur[n_ev] = e->duration_ms; }
    n_ev++;
}

/* Run seconds of signal: background noise-like 1000 Hz level bg(t), plus 800 ms
 * markers of amplitude 1 starting at first_ms + k*60000. */
static void run(marker_detector_t *md, double seconds, double first_ms, double step_ms, double step_level) {
    unsigned r = 7;
    for (long n = 0; n < (long)(seconds * FS); n++) {
        double t = n / FS, tms = t * 1000.0;
        double bg = (step_ms > 0 && tms >= step_ms) ? step_level : 0.05;
        double k = floor((tms - first_ms) / 60000.0), ph = tms - first_ms - k * 60000.0;
        double a = (k >= 0 && ph >= 0 && ph < 800.0) ? 1.0 : 0.0;
        r = r * 1103515245u + 12345u;
        double nz = ((r >> 8) / 16777216.0 - 0.5) * 0.02;
        double s = (a + bg) * sin(2 * M_PI * 1000 * t) + nz;
        double c = (a + bg) * cos(2 * M_PI * 1000 * t) + nz;
        marker_detector_process_sample(md, (float)s, (float)c);
    }
}

int main(void) {
    int fails = 0;

    /* 1. Edges */
    marker_detector_t *md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    n_ev = 0;
    run(md, 200.0, 20000.0, 0, 0);
    int ok_edges = 0;
    for (int i = 0; i < n_ev && i < 32; i++) {
        double truth = 20000.0 + 60000.0 * i;
        double err = ev_start[i] - truth;
        int ok = fabs(err) <= 30.0 && fabs(ev_dur[i] - 800.0) <= 100.0;
        printf("%-4s edge: marker %d start err %+.1f ms, length %.0f ms\n", ok ? "ok" : "FAIL", i + 1, err, ev_dur[i]);
        ok_edges += ok;
    }
    if (n_ev != 3 || ok_edges != 3) { printf("FAIL edges: %d events, %d within tolerance (want 3)\n", n_ev, ok_edges); fails++; }
    marker_detector_destroy(md);

    /* 2. Timeout reset: background steps to 0.2 at 50 s; markers every 60 s from 20 s. */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    n_ev = 0;
    run(md, 320.0, 20000.0, 50000.0, 0.2);
    int late = 0;
    for (int i = 0; i < n_ev && i < 32; i++) if (ev_start[i] > 100000.0) late++;
    /* markers at 140, 200, 260 s follow the step and the timeout */
    printf("%-4s timeout reset: %d of 3 markers after the step detected (%d events total)\n",
           late >= 3 ? "ok" : "FAIL", late, n_ev);
    if (late < 3) fails++;
    marker_detector_destroy(md);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
