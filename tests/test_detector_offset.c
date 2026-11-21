/* Detector stream time offset: the tick, marker and BCD detectors stamp the same
 * signal 40 h later as exactly 40 h later.
 *
 * One synthetic 125 s signal (the WWV format of test_chain_synth.c, with a 300 ms
 * 1000 Hz burst at :45 for a tick-path candidate) goes through the channel filters
 * into fresh tick, marker, BCD time and BCD freq detectors, twice. Each run starts
 * the detectors at a frame count: a base of 20 x 1024 ms, then that base plus 40 h.
 * Both are whole frames at 5.12 ms (tick, marker, BCD time) and at 40.96 ms (BCD
 * freq). The base puts both runs past the detectors' startup guards, which key on
 * stream time from 0 (MARKER_MIN_STARTUP_MS 10 s, BCD_FREQ_MIN_STARTUP_MS 5 s), so
 * the two runs see the same detector logic. Warmup counts from start_frame and is
 * the same in both.
 *
 * Every event's stream times (tick, tick-path marker, marker detector, tick-path
 * candidate, BTIME; marker candidates and BFREQ too, though this signal makes none)
 * must equal the base run's plus exactly
 * 144,000,000 ms, and every width must be equal. The stamps come from
 * wwv_frames_to_ms(), so this fails if that returns float (16 ms step at 40 h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channel_filters.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"

#define FS        50000.0
#define SECONDS   125
#define BASE_MS   (20.0 * 1024.0)
#define OFFSET_MS (40.0 * 3600.0 * 1000.0)

typedef struct { char kind; int nt; double t[4]; float w; } ev_t;
#define MAX_EV 2000
static ev_t evs[2][MAX_EV];
static int nev[2], cur;
static int truth[SECONDS + 2];

static void rec(char kind, int nt, double a, double b, double c, double d, float w) {
    if (nev[cur] >= MAX_EV) return;
    evs[cur][nev[cur]++] = (ev_t){ kind, nt, { a, b, c, d }, w };
}
static void on_tick(const tick_event_t *e, void *u) { (void)u; rec('T', 1, e->timestamp_ms, 0, 0, 0, e->duration_ms); }
static void on_tmark(const tick_marker_event_t *e, void *u) {
    (void)u; rec('M', 2, e->timestamp_ms, e->start_timestamp_ms, 0, 0, e->duration_ms);
}
static void on_tcand(const tick_marker_candidate_t *c, void *u) {
    (void)u; rec('C', 4, c->start_ms, c->end_ms, c->lead_from_start_ms, c->lead_from_end_ms, 0.0f);
}
static void on_mark(const marker_event_t *e, void *u) {
    (void)u; rec('K', 2, e->timestamp_ms, e->start_timestamp_ms, 0, 0, e->duration_ms);
}
static void on_mcand(const marker_event_t *e, void *u) {
    (void)u; rec('k', 2, e->timestamp_ms, e->start_timestamp_ms, 0, 0, e->duration_ms);
}
static void on_bt(const bcd_time_event_t *e, void *u) { (void)u; rec('B', 1, e->timestamp_ms, 0, 0, 0, e->duration_ms); }
static void on_bf(const bcd_freq_event_t *e, void *u) { (void)u; rec('F', 1, e->timestamp_ms, 0, 0, 0, e->duration_ms); }

/* Start a fresh detector at frame `f`: its count, and the frames that count from
 * it (warmup, logging, the BCD level hold). Fields that are 0 for "none yet"
 * stay 0; each is set before it is read. One file per detector
 * (detector_offset_start_*.c): their internal headers cannot share one. */
void detector_offset_start_tick(tick_detector_t *d, uint64_t f);
void detector_offset_start_marker(marker_detector_t *d, uint64_t f);
void detector_offset_start_btime(bcd_time_detector_t *d, uint64_t f);
void detector_offset_start_bfreq(bcd_freq_detector_t *d, uint64_t f);

static void run(double start_ms) {
    nev[cur] = 0;
    tick_detector_t *tk = tick_detector_create(NULL);
    marker_detector_t *mk = marker_detector_create(NULL);
    bcd_time_detector_t *bt = bcd_time_detector_create(NULL);
    bcd_freq_detector_t *bf = bcd_freq_detector_create(NULL);
    if (!tk || !mk || !bt || !bf) { fprintf(stderr, "create failed\n"); exit(2); }
    /* whole frames: checked in main() */
    detector_offset_start_tick(tk, (uint64_t)(start_ms * TICK_SAMPLE_RATE / 1000.0 / TICK_FFT_SIZE));
    detector_offset_start_marker(mk, (uint64_t)(start_ms * MARKER_SAMPLE_RATE / 1000.0 / MARKER_FFT_SIZE));
    detector_offset_start_btime(bt, (uint64_t)(start_ms * BCD_TIME_SAMPLE_RATE / 1000.0 / BCD_TIME_FFT_SIZE));
    detector_offset_start_bfreq(bf, (uint64_t)(start_ms * BCD_FREQ_SAMPLE_RATE / 1000.0 / BCD_FREQ_FFT_SIZE));
    tick_detector_set_callback(tk, on_tick, NULL);
    tick_detector_set_marker_callback(tk, on_tmark, NULL);
    tick_detector_set_candidate_callback(tk, on_tcand, NULL);
    marker_detector_set_callback(mk, on_mark, NULL);
    marker_detector_set_candidate_callback(mk, on_mcand, NULL);
    bcd_time_detector_set_callback(bt, on_bt, NULL);
    bcd_freq_detector_set_callback(bf, on_bf, NULL);

    sync_channel_t si, sq; data_channel_t di, dq;
    sync_channel_init(&si); sync_channel_init(&sq); data_channel_init(&di); data_channel_init(&dq);
    const long total = (long)(SECONDS * FS);
    const double t0 = 59.5;
    unsigned noise = 1;
    for (long n = 0; n < total; n++) {
        double t = t0 + n / FS;
        int s = (int)floor(t);
        double frac = t - s;
        int sec = s % 60;
        double a = 0.0;
        if (sec == 0) { if (frac < 0.800) a += sin(2 * M_PI * 1000 * t); }
        else if (sec == 45) { if (frac < 0.005 || (frac >= 0.300 && frac < 0.600)) a += sin(2 * M_PI * 1000 * t); }
        else if (sec != 29 && sec != 59 && frac < 0.005) a += sin(2 * M_PI * 1000 * t);
        int ts = (int)floor(n / FS + 0.5 - 1e-9);
        int sym = (ts >= 0 && ts <= SECONDS) ? truth[ts] : -1;
        double hi_ms = sym == 0 ? 200 : sym == 1 ? 500 : sym == 2 ? 800 : 0;
        double level = (sym >= 0 && frac >= 0.030 && frac * 1000 < hi_ms) ? 1.0 : 0.15;
        a += 0.5 * level * sin(2 * M_PI * 100 * t);
        noise = noise * 1103515245u + 12345u;
        double nz = ((noise >> 8) / 16777216.0 - 0.5) * 0.02;
        float I = (float)(1.0 + 0.5 * a + nz), Q = 0.0f;
        float s_i = sync_channel_process(&si, I), s_q = sync_channel_process(&sq, Q);
        float d_i = data_channel_process(&di, I), d_q = data_channel_process(&dq, Q);
        tick_detector_process_sample(tk, s_i, s_q);
        marker_detector_process_sample(mk, s_i, s_q);
        bcd_time_detector_process_sample(bt, d_i, d_q);
        bcd_freq_detector_process_sample(bf, d_i, d_q);
    }
    tick_detector_destroy(tk);
    marker_detector_destroy(mk);
    bcd_time_detector_destroy(bt);
    bcd_freq_detector_destroy(bf);
}

static int count(int r, char k) { int n = 0; for (int i = 0; i < nev[r]; i++) n += evs[r][i].kind == k; return n; }

int main(void) {
    /* The detectors' own console output is not the result */
    if (!freopen("/dev/null", "w", stdout)) return 2;
    srand(12345);
    for (int s = 0; s <= SECONDS + 1; s++) {
        int sec = s % 60;
        truth[s] = (sec == 0) ? -1 : (sec % 10 == 9) ? 2 : (rand() & 1);
    }
    int fail = 0;
    double starts[2] = { BASE_MS, BASE_MS + OFFSET_MS };
    for (int r = 0; r < 2; r++) {
        double f5 = starts[r] / 5.12, f40 = starts[r] / 40.96;
        if (f5 != floor(f5) || f40 != floor(f40)) { fprintf(stderr, "FAIL: start %.3f ms is not whole frames\n", starts[r]); return 2; }
        cur = r;
        run(starts[r]);
    }

    fprintf(stderr, "base run: %d events: ticks %d, tick markers %d, tick candidates %d, markers %d, "
            "marker candidates %d, BTIME %d, BFREQ %d\n", nev[0], count(0, 'T'), count(0, 'M'), count(0, 'C'),
            count(0, 'K'), count(0, 'k'), count(0, 'B'), count(0, 'F'));
    const char kinds[] = "TMCKB";
    for (const char *k = kinds; *k; k++)
        if (count(0, *k) == 0) { fprintf(stderr, "FAIL: the base run has no '%c' event\n", *k); fail = 1; }

    if (nev[0] != nev[1]) { fprintf(stderr, "FAIL: %d events at the base, %d at 40 h\n", nev[0], nev[1]); fail = 1; }
    int n = nev[0] < nev[1] ? nev[0] : nev[1], bad = 0;
    double worst = 0.0;
    for (int i = 0; i < n; i++) {
        const ev_t *a = &evs[0][i], *b = &evs[1][i];
        int ok = a->kind == b->kind && a->w == b->w;
        for (int j = 0; j < a->nt; j++) {
            double d = b->t[j] - (a->t[j] + OFFSET_MS);
            if (d != 0.0) ok = 0;
            if (fabs(d) > worst) worst = fabs(d);
        }
        if (!ok && bad++ < 5)
            fprintf(stderr, "  event %d '%c': base t=%.6f w=%.3f, 40 h '%c' t-40h=%.6f w=%.3f\n", i, a->kind,
                    a->t[0], a->w, b->kind, b->t[0] - OFFSET_MS, b->w);
    }
    fprintf(stderr, "events whose 40 h stamps are not the base's plus exactly 40 h: %d of %d (largest error %.6f ms)\n",
            bad, n, worst);
    if (bad) fail = 1;
    fprintf(stderr, fail ? "FAILED\n" : "PASSED\n");
    return fail;
}
