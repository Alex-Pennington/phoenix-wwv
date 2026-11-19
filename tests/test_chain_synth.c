/* Synthetic end-to-end smoke test for the detector chain.
 *
 * Builds 125 s of 50 kHz complex baseband in the WWV format: a carrier at DC plus
 * 0.5 x audio, where the audio is a 5 ms 1000 Hz tick each second except :00/:29/:59,
 * an 800 ms 1000 Hz minute marker at :00, and a 100 Hz BCD subcarrier (high level
 * for 200/500/800 ms after +30 ms, low level otherwise). Wiring is the same as
 * tests/air/harness_iq.c: channel filters -> tick, marker, BCD time/freq detectors
 * -> sync detector -> BCD correlator.
 *
 * This is a model, not a recording. It guards the build and gross regressions;
 * on-air performance is measured with tests/air/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channel_filters.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
#include "sync_detector.h"

#define FS 50000.0
#define SECONDS 125

static int n_tick, n_tmark, n_mark, n_btime, n_bfreq, n_sym, n_sym_ok;
static int truth[SECONDS + 2];  /* symbol per second: 0, 1, 2 = P, -1 = none */
static sync_detector_t *g_sync;
static tick_detector_t *g_tick;

static void on_tick(const tick_event_t *e, void *u) { (void)e; (void)u; n_tick++; }
static void on_tick_marker(const tick_marker_event_t *e, void *u) {
    (void)u; n_tmark++;
    sync_detector_tick_marker(g_sync, e->start_timestamp_ms, e->duration_ms, e->corr_ratio);
    if (tick_detector_get_epoch_source(g_tick) == EPOCH_SOURCE_NONE)
        tick_detector_set_epoch_with_source(g_tick, e->start_timestamp_ms, EPOCH_SOURCE_MARKER, 0.7f);
}
static void on_marker(const marker_event_t *e, void *u) {
    (void)u; n_mark++;
    sync_detector_marker_event(g_sync, e->timestamp_ms, e->accumulated_energy, e->duration_ms);
}
static void on_bt(const bcd_time_event_t *e, void *u) {
    n_btime++;
    bcd_correlator_time_event((bcd_correlator_t *)u, e->timestamp_ms, e->duration_ms, e->peak_energy);
}
static void on_bf(const bcd_freq_event_t *e, void *u) {
    n_bfreq++;
    bcd_correlator_freq_event((bcd_correlator_t *)u, e->timestamp_ms, e->duration_ms, e->accumulated_energy);
}
static void on_sym(const bcd_symbol_event_t *e, void *u) {
    (void)u;
    char c = bcd_corr_symbol_char(e->symbol);
    if (c != '0' && c != '1' && c != 'P') return;
    n_sym++;
    int s = (int)floor(e->timestamp_ms / 1000.0);
    if (s >= 0 && s <= SECONDS) {
        int t = truth[s];
        if ((t == 0 && c == '0') || (t == 1 && c == '1') || (t == 2 && c == 'P')) n_sym_ok++;
    }
}

int main(int argc, char **argv) {
    int verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    srand(12345);
    for (int s = 0; s <= SECONDS + 1; s++) {
        int sec = s % 60;
        truth[s] = (sec == 0) ? -1 : (sec % 10 == 9) ? 2 : (rand() & 1);
    }

    g_tick = tick_detector_create(NULL);
    marker_detector_t *mark = marker_detector_create(NULL);
    g_sync = sync_detector_create(NULL);
    bcd_correlator_t *bcd = bcd_correlator_create(NULL);
    bcd_time_detector_t *bt = bcd_time_detector_create(NULL);
    bcd_freq_detector_t *bf = bcd_freq_detector_create(NULL);
    if (!g_tick || !mark || !g_sync || !bcd || !bt || !bf) { fprintf(stderr, "create failed\n"); return 1; }
    tick_detector_set_callback(g_tick, on_tick, NULL);
    tick_detector_set_marker_callback(g_tick, on_tick_marker, NULL);
    marker_detector_set_callback(mark, on_marker, NULL);
    bcd_time_detector_set_callback(bt, on_bt, bcd);
    bcd_freq_detector_set_callback(bf, on_bf, bcd);
    bcd_correlator_set_sync_source(bcd, g_sync);
    bcd_correlator_set_callback(bcd, on_sym, NULL);

    sync_channel_t si, sq; data_channel_t di, dq;
    sync_channel_init(&si); sync_channel_init(&sq); data_channel_init(&di); data_channel_init(&dq);

    /* Start 0.5 s before a minute so the first marker is complete. */
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
        else if (sec != 29 && sec != 59 && frac < 0.005) a += sin(2 * M_PI * 1000 * t);
        /* BCD for the current second: truth index is relative to sample 0. */
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
        tick_detector_process_sample(g_tick, s_i, s_q);
        marker_detector_process_sample(mark, s_i, s_q);
        bcd_time_detector_process_sample(bt, d_i, d_q);
        bcd_freq_detector_process_sample(bf, d_i, d_q);
        if (n % 2500 == 0) sync_detector_periodic_check(g_sync, (float)(n / FS * 1000.0));
    }
    (void)verbose;
    printf("synth: ticks=%d tick_markers=%d markers=%d btime=%d bfreq=%d symbols=%d correct=%d\n",
           n_tick, n_tmark, n_mark, n_btime, n_bfreq, n_sym, n_sym_ok);
    bcd_correlator_destroy(bcd);
    bcd_time_detector_destroy(bt);
    bcd_freq_detector_destroy(bf);
    marker_detector_destroy(mark);
    tick_detector_destroy(g_tick);
    sync_detector_destroy(g_sync);

    int fail = 0;
    /* 125 s holds about 120 tick seconds; demand most of them. */
    if (n_tick < 90) { printf("FAIL: ticks %d < 90\n", n_tick); fail = 1; }
    if (n_mark + n_tmark < 2) { printf("FAIL: no minute markers\n"); fail = 1; }
    /* BCD with the carrier present: about 123 pulse seconds; the carrier must not
     * swamp the BCD time detector. */
    if (n_btime < 90) { printf("FAIL: BCD time pulses %d < 90 (carrier at DC)\n", n_btime); fail = 1; }
    printf(fail ? "FAILED\n" : "PASSED\n");
    return fail;
}
