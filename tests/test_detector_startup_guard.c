/* Detector startup guards count from the detector's own start, not stream time 0.
 *
 * The marker detector suppresses events for MARKER_MIN_STARTUP_MS (10 s) after
 * warmup and the BCD freq detector for BCD_FREQ_MIN_STARTUP_MS (5 s); both learn
 * their baseline meanwhile. One synthetic 90 s signal goes into fresh detectors
 * started at frame 0 and at an offset of about 1 h (OFFSET_MS: whole frames at
 * 5.12 ms and 40.96 ms; exactly 1 h is not a whole 40.96 ms frame):
 *   - 800 ms 1000 Hz markers at 3, 6, 9 s (inside the marker guard) and 15, 75 s;
 *   - 400 ms 100 Hz bursts every 3 s from 2 s on, alone on a weak 100 Hz floor
 *     (the BCD freq guard ends at 5 s).
 * At frame 0 and at the offset the detectors must give the same events, the
 * offset run's exactly OFFSET_MS later: none inside either guard, and both
 * kinds after it. Before the guards counted from start_frame, the run started
 * at the offset had no guard at all (stream time was past 10 s from its first
 * frame) and reported the in-guard markers and bursts.
 *
 * The detectors start at the offset through tests/detector_offset_start_*.c, as
 * in test_detector_offset.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channel_filters.h"
#include "marker_detector.h"
#include "bcd_freq_detector.h"

#define FS          50000.0
#define SECONDS     90
#define OFFSET_MS   (87890.0 * 40.96)      /* 3,599,974.4 ms */
#define MARKER_GUARD_MS   10000.0
#define BFREQ_GUARD_MS    5000.0
#ifndef BURST_AMP
#define BURST_AMP         6.0     /* over the floor: a run of the 1 s sum within 100-1000 ms */
#endif

typedef struct { char kind; double t; float w; } ev_t;
#define MAX_EV 500
static ev_t evs[2][MAX_EV];
static int nev[2], cur;

static void rec(char kind, double t, float w) {
    if (nev[cur] < MAX_EV) evs[cur][nev[cur]++] = (ev_t){ kind, t, w };
}
static void on_mark(const marker_event_t *e, void *u) { (void)u; rec('K', e->timestamp_ms, e->duration_ms); }
static void on_mcand(const marker_event_t *e, void *u) { (void)u; rec('k', e->timestamp_ms, e->duration_ms); }
static void on_bf(const bcd_freq_event_t *e, void *u) { (void)u; rec('F', e->timestamp_ms, e->duration_ms); }

void detector_offset_start_marker(marker_detector_t *d, uint64_t f);
void detector_offset_start_bfreq(bcd_freq_detector_t *d, uint64_t f);

static const double MARKERS_S[] = { 2.5, 5, 7.5, 15, 75 };
static const double BURSTS_S[] = { 2, 3.5, 8, 11, 14, 17, 20 };

static void run(double start_ms) {
    nev[cur] = 0;
    marker_detector_t *mk = marker_detector_create(NULL);
    bcd_freq_detector_t *bf = bcd_freq_detector_create(NULL);
    if (!mk || !bf) { fprintf(stderr, "create failed\n"); exit(2); }
    detector_offset_start_marker(mk, (uint64_t)llround(start_ms / 5.12));
    detector_offset_start_bfreq(bf, (uint64_t)llround(start_ms / 40.96));
    marker_detector_set_callback(mk, on_mark, NULL);
    marker_detector_set_candidate_callback(mk, on_mcand, NULL);
    bcd_freq_detector_set_callback(bf, on_bf, NULL);

    sync_channel_t si, sq; data_channel_t di, dq;
    sync_channel_init(&si); sync_channel_init(&sq); data_channel_init(&di); data_channel_init(&dq);
    unsigned noise = 1;
    for (long n = 0; n < (long)(SECONDS * FS); n++) {
        double t = n / FS;
        double a = 0.0;
        for (size_t k = 0; k < sizeof MARKERS_S / sizeof MARKERS_S[0]; k++)
            if (t >= MARKERS_S[k] && t < MARKERS_S[k] + 0.800) a += sin(2 * M_PI * 1000 * t);
        double lvl = 0.05;
        for (size_t k = 0; k < sizeof BURSTS_S / sizeof BURSTS_S[0]; k++)
            if (t >= BURSTS_S[k] && t < BURSTS_S[k] + 0.400) lvl = 0.05 * BURST_AMP;
        a += 0.5 * lvl * sin(2 * M_PI * 100 * t);
        noise = noise * 1103515245u + 12345u;
        double nz = ((noise >> 8) / 16777216.0 - 0.5) * 0.02;
        float I = (float)(1.0 + 0.5 * a + nz), Q = 0.0f;
        float s_i = sync_channel_process(&si, I), s_q = sync_channel_process(&sq, Q);
        float d_i = data_channel_process(&di, I), d_q = data_channel_process(&dq, Q);
        marker_detector_process_sample(mk, s_i, s_q);
        bcd_freq_detector_process_sample(bf, d_i, d_q);
    }
    marker_detector_destroy(mk);
    bcd_freq_detector_destroy(bf);
}

static int fail;
static void check(int ok, const char *fmt, double a, double b) {
    fprintf(stderr, "%-4s ", ok ? "ok" : "FAIL");
    fprintf(stderr, fmt, a, b);
    fprintf(stderr, "\n");
    if (!ok) fail = 1;
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    if (!freopen("/dev/null", "w", stdout)) return 2;
    double f5 = OFFSET_MS / 5.12, f40 = OFFSET_MS / 40.96;
    if (fabs(f5 - llround(f5)) > 1e-6 || fabs(f40 - llround(f40)) > 1e-6) {
        fprintf(stderr, "FAIL: offset %.3f ms is not whole frames\n", OFFSET_MS);
        return 2;
    }
    double starts[2] = { 0.0, OFFSET_MS };
    for (int r = 0; r < 2; r++) { cur = r; run(starts[r]); }

    for (int r = 0; r < 2; r++) {
        int first_k = 0, first_f = 0, nk = 0, nf = 0;
        double ek = INFINITY, ef = INFINITY;
        for (int i = 0; i < nev[r]; i++) {
            double t = evs[r][i].t - starts[r];
            if (verbose) fprintf(stderr, "  run %d: '%c' %.3f s w=%.1f\n", r, evs[r][i].kind, t / 1000.0, evs[r][i].w);
            if (evs[r][i].kind == 'K' || evs[r][i].kind == 'k') { nk++; if (t < ek) ek = t; first_k = 1; }
            if (evs[r][i].kind == 'F') { nf++; if (t < ef) ef = t; first_f = 1; }
        }
        const char *at = r ? "1 h" : "0 h";
        fprintf(stderr, "     %s: %d marker events (first at %.3f s), %d BFREQ (first at %.3f s)\n",
                at, nk, ek / 1000.0, nf, ef / 1000.0);
        char fmt[128];
        snprintf(fmt, sizeof fmt, "%s: no marker event in the first %%.0f s of the detector (first at %%.3f s)", at);
        check(first_k && ek >= MARKER_GUARD_MS, fmt, MARKER_GUARD_MS / 1000.0, ek / 1000.0);
        snprintf(fmt, sizeof fmt, "%s: no BFREQ event in the first %%.0f s of the detector (first at %%.3f s)", at);
        check(first_f && ef >= BFREQ_GUARD_MS, fmt, BFREQ_GUARD_MS / 1000.0, ef / 1000.0);
        snprintf(fmt, sizeof fmt, "%s: the markers after the guard are reported (%%.0f events, want %%.0f)", at);
        check(nk == 2, fmt, nk, 2);
    }

    int same = nev[0] == nev[1];
    for (int i = 0; same && i < nev[0]; i++)
        same = evs[0][i].kind == evs[1][i].kind && evs[0][i].w == evs[1][i].w &&
               fabs(evs[1][i].t - OFFSET_MS - evs[0][i].t) < 1e-6;
    check(same, "the 1 h run's events are the 0 h run's, %.0f of them, %.1f ms later (to 1e-6 ms)", nev[0], OFFSET_MS);
    fprintf(stderr, fail ? "FAILED\n" : "PASSED\n");
    return fail;
}
