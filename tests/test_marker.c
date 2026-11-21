/* Marker detector tests on a synthetic 1000 Hz signal at the sync-channel output.
 *
 * 1. Edges: 800 ms markers at a known time. The leading edge must land within
 *    +-30 ms of truth and the length estimate within 800 +-100 ms; the raw window
 *    run is 1.6-1.8 s.
 * 2. Timeout reset: the in-band level steps up 4x and stays there (a tone, a
 *    fade-up). The detector times out once; without the baseline reset it keeps
 *    timing out and misses every later marker.
 * 3. Voice: broadband bursts (band noise, 200 ms on / 100 ms off for 4 s, from
 *    7.5 s before each marker, as the :52.5 announcement) strong enough to trip the
 *    1000 Hz window sum. None may count as a marker or a candidate, and the real
 *    marker 3.5 s after each burst must still be found.
 * 4. Weak marker: a marker 1.1x the weakest this detector finds in band noise
 *    (amplitude 0.11 at noise 0.1; 0.105 is not detected) must still count.
 * 5. Short run: a 1000 ms in-band blip that ends 13 s before each marker and
 *    lifts the window sum just over the threshold (a 343 ms window run) must not
 *    hide the marker. Fails before the short run stopped taking the cooldown.
 * 6. Weak marker candidates: markers at amplitude 0.10 in noise 0.10 are not
 *    found as markers, but each gives a short run reported as a candidate within
 *    30 ms of its leading edge. Case 3 also checks the voice bursts give none.
 * 7. Voice short runs: weaker voice bursts (no markers) give short runs, which
 *    fail the purity test and so give no candidate.
 * 8. Purity rejections reach telemetry (TELEM_MARKERS "PURITY," lines, which the
 *    chain log writes): case 3's voice bursts give one per rejection.
 * Cases 3 and 4 run white noise through the sync channel filter, as on air.
 */
#include <stdio.h>
#include <math.h>
#include "marker_detector.h"
#include "channel_filters.h"
#include "telemetry.h"
#include <string.h>

static int n_purity;
static void purity_tap(telem_channel_t ch, const char *line, void *u) {
    (void)u;
    if (ch == TELEM_MARKERS && strncmp(line, "PURITY,", 7) == 0) n_purity++;
}

#define FS 50000.0
#define BLIP_A 0.16   /* window run 343 ms, peak 3.44x baseline: a short run */

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

/* As run(), no step, plus a blip of amplitude blip_a and length blip_ms ending
 * blip_before_ms before each marker. */
static void run_blip(marker_detector_t *md, double seconds, double first_ms,
                     double blip_before_ms, double blip_ms, double blip_a) {
    unsigned r = 7;
    for (long n = 0; n < (long)(seconds * FS); n++) {
        double t = n / FS, tms = t * 1000.0;
        double k = floor((tms - first_ms) / 60000.0), ph = tms - first_ms - k * 60000.0;
        double a = (k >= 0 && ph >= 0 && ph < 800.0) ? 1.0 : 0.0;
        double pb = ph - (60000.0 - blip_before_ms - blip_ms);
        if (pb >= 0 && pb < blip_ms) a = blip_a;
        r = r * 1103515245u + 12345u;
        double nz = ((r >> 8) / 16777216.0 - 0.5) * 0.02;
        double s = (a + 0.05) * sin(2 * M_PI * 1000 * t) + nz;
        double c = (a + 0.05) * cos(2 * M_PI * 1000 * t) + nz;
        marker_detector_process_sample(md, (float)s, (float)c);
    }
}

static int n_cand; static float cand_start[32];
static void on_candidate(const marker_event_t *e, void *u) {
    (void)u;
    if (n_cand < 32) cand_start[n_cand] = e->start_timestamp_ms;
    n_cand++;
}

static unsigned nr = 12345;
static double gauss(void) {
    double s = 0;
    for (int i = 0; i < 12; i++) { nr = nr * 1103515245u + 12345u; s += (nr >> 8) / 16777216.0; }
    return s - 6.0;
}

/* Markers of amplitude a at first_ms + k*60 s in white noise of sigma noise, all
 * through the sync channel filter. voice > 0 adds noise bursts of that sigma. */
static void run_noise(marker_detector_t *md, double seconds, double first_ms, double a,
                      double noise, double voice) {
    sync_channel_t si, sq;
    sync_channel_init(&si); sync_channel_init(&sq);
    nr = 12345;
    for (long n = 0; n < (long)(seconds * FS); n++) {
        double t = n / FS, tms = t * 1000.0;
        double k = floor((tms - first_ms) / 60000.0), ph = tms - first_ms - k * 60000.0;
        double m = (k >= 0 && ph >= 0 && ph < 800.0) ? a : 0.0;
        double pv = tms - first_ms + 7500.0, kv = floor(pv / 60000.0);
        pv -= kv * 60000.0;
        double g = (voice > 0 && kv >= 0 && pv < 4000.0 && fmod(pv, 300.0) < 200.0) ? voice : 0.0;
        double I = m * cos(2 * M_PI * 1000 * t) + noise * gauss() + g * gauss();
        double Q = m * sin(2 * M_PI * 1000 * t) + noise * gauss() + g * gauss();
        marker_detector_process_sample(md, sync_channel_process(&si, (float)I),
                                       sync_channel_process(&sq, (float)Q));
    }
}

/* Events within +-50 ms of the markers at first_ms + k*60 s; *stray counts the rest. */
static int on_time(double first_ms, int *stray) {
    int hit = 0; *stray = 0;
    for (int i = 0; i < n_ev && i < 32; i++) {
        double k = floor((ev_start[i] - first_ms) / 60000.0 + 0.5);
        if (fabs(ev_start[i] - first_ms - 60000.0 * k) <= 50.0) hit++; else (*stray)++;
    }
    return hit;
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

    /* 3. Voice bursts at 12.5, 72.5, 132.5, 192.5 s; markers at 20, 80, 140 s.
     *    The bursts must give no marker candidate either (the purity test). */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    marker_detector_set_candidate_callback(md, on_candidate, NULL);
    n_ev = 0; n_cand = 0; n_purity = 0;
    telem_set_tap(purity_tap, NULL);
    run_noise(md, 200.0, 20000.0, 0.3, 0.1, 0.6);
    telem_set_tap(NULL, NULL);
    int stray, hit = on_time(20000.0, &stray);
    printf("%-4s purity rejections on telemetry: %d\n", n_purity > 0 ? "ok" : "FAIL", n_purity);
    if (n_purity == 0) fails++;
    printf("%-4s voice: %d of 3 markers found, %d events and %d candidates on voice bursts\n",
           (hit == 3 && stray == 0 && n_cand == 0) ? "ok" : "FAIL", hit, stray, n_cand);
    if (hit != 3 || stray != 0 || n_cand != 0) fails++;
    marker_detector_destroy(md);

    /* 4. Weak markers at 20, 80, 140 s. */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    n_ev = 0;
    run_noise(md, 200.0, 20000.0, 0.12, 0.1, 0.0);
    hit = on_time(20000.0, &stray);
    printf("%-4s weak marker: %d of 3 found, %d stray\n", (hit == 3 && stray == 0) ? "ok" : "FAIL", hit, stray);
    if (hit != 3 || stray != 0) fails++;
    marker_detector_destroy(md);

    /* 5. A short run does not hide the next marker: a 1000 ms blip that lifts
     * the window sum just over the threshold (a window run of about 300 ms,
     * under min_duration_ms) ends 13 s before each of the markers at 80 and
     * 140 s. The 30 s cooldown it used to take hid both. */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    n_ev = 0;
    run_blip(md, 200.0, 20000.0, 13000.0, 1000.0, BLIP_A);
    hit = on_time(20000.0, &stray);
    printf("%-4s short run: %d of 3 markers found, %d stray\n", (hit == 3 && stray == 0) ? "ok" : "FAIL", hit, stray);
    if (hit != 3 || stray != 0) fails++;
    marker_detector_destroy(md);

    /* 6. Weaker markers (amplitude 0.10, under the 0.105 case 4 says is not
     * found) at 20 + 60k s: no marker, but each gives a short run whose
     * midpoint places it, reported as a candidate. */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    marker_detector_set_candidate_callback(md, on_candidate, NULL);
    n_ev = 0; n_cand = 0;
    run_noise(md, 380.0, 20000.0, 0.10, 0.1, 0.0);
    int cand_ok = 0;
    for (int i = 0; i < n_cand && i < 32; i++) {
        double k = floor((cand_start[i] - 20000.0) / 60000.0 + 0.5);
        if (fabs(cand_start[i] - 20000.0 - 60000.0 * k) <= 30.0) cand_ok++;
    }
    printf("%-4s weak marker candidates: %d of 6 within 30 ms, %d candidates, %d markers\n",
           (cand_ok == 6 && n_cand == 6 && n_ev == 0) ? "ok" : "FAIL", cand_ok, n_cand, n_ev);
    if (cand_ok != 6 || n_cand != 6 || n_ev != 0) fails++;
    marker_detector_destroy(md);

    /* 7. Voice alone, weaker (sigma 0.5): its bursts clear the window threshold
     * only briefly, giving short runs at :52.5-:56 (7 in 380 s, flank/centre
     * 0.97-1.11). They must give no candidate: a short run is a candidate only
     * if it passes the purity test. */
    md = marker_detector_create(NULL);
    marker_detector_set_callback(md, on_marker, NULL);
    marker_detector_set_candidate_callback(md, on_candidate, NULL);
    n_ev = 0; n_cand = 0;
    run_noise(md, 380.0, 20000.0, 0.0, 0.1, 0.5);
    printf("%-4s voice short runs: %d candidates, %d markers\n", (n_cand == 0 && n_ev == 0) ? "ok" : "FAIL", n_cand, n_ev);
    if (n_cand != 0 || n_ev != 0) fails++;
    marker_detector_destroy(md);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
