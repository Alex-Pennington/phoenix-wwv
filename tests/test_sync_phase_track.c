/* Sync phase tracking under lost samples and false ticks, through the whole chain.
 *
 * An event-level WWV signal (as tests/air/replay_events feeds a log) runs through
 * the manager's own routing: sync detector, BCD correlator and time decoder. Each
 * true second carries a tick at +15 ms (none at :00, :29, :59), a BCD time pulse
 * from +35 ms whose width is the second's symbol in the true NIST frame (from
 * 14:00 UTC 2026-324), and false ticks at random times. The minute marker is a
 * tick-path marker at :00, reported at its end.
 *
 * Lost samples: from 12.5 minutes, DROP_MS of stream is missing. Events inside it
 * vanish and every later event is DROP_MS early in stream time, as when
 * libusb drops a buffer (run90g, run90d). Markers are missed for minutes 12-17,
 * so only the edges, and in some scenarios a marker candidate (a run the tick
 * detector rejected: its leading edge is the true :00), can show the loss before
 * the minute-18 marker.
 *
 * Checks, for each scenario:
 *   - no time of day decoded wrong, in any state;
 *   - LOCKED at the end, with the held phase within 30 ms of the truth, and
 *     within 30 ms whenever LOCKED from the first marker after the drop (or 3
 *     minutes after a burst);
 *   - time of day CONFIRMED right after minute 19;
 *   - symbols emitted >140 ms off their true second, frames admitted holding
 *     one (the frame that closes before the loss can be seen), and results
 *     decoded from a chain holding such a frame: at most the scenario's bounds.
 * Scenarios: drops of 300, 1000, 1050 (run90d's whole-second-plus-50 ms loss)
 * and 1400 ms, with a candidate in the minutes whose marker is missed; 1400 ms
 * with none (nothing shows the whole second until the minute-18 marker: the
 * limit); a continuous loss of 3 ms/s for 5 minutes (run90g seg2's shape); and
 * with no drop, a 40 s burst of random false ticks and a 30 s burst of false
 * ticks 300 ms late.
 *
 * The same events through the chain before the phase tracker (8b673ef):
 * symbols >140 ms off 321 / 314 / 314 / 321 / 321 for the five drops,
 * admitted frames holding them 7 / 2 / 2 / 5 / 5, results from such chains
 * 17 / 17 / 6 / 6 / 6, and no CONFIRMED time of day after minute 19 for the
 * 300 ms drop. The ramp: 273 symbols off, 2 frames admitted holding them, 16
 * results from such chains.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "wwv_detector_manager.h"
#include "manager/wwv_detector_manager_internal.h"
#include "wwv_time_decoder.h"
#include "telemetry.h"

/* ---- telemetry: silent ---- */
bool telem_init(int port) { (void)port; return true; }
void telem_cleanup(void) {}
void telem_enable(uint32_t c) { (void)c; }
void telem_disable(uint32_t c) { (void)c; }
void telem_set_channels(uint32_t c) { (void)c; }
bool telem_is_enabled(telem_channel_t c) { (void)c; return false; }
void telem_send(telem_channel_t c, const char *l) { (void)c; (void)l; }
void telem_sendf(telem_channel_t c, const char *fmt, ...) { (void)c; (void)fmt; }
void telem_get_stats(uint32_t *s, uint32_t *d) { if (s) *s = 0; if (d) *d = 0; }
void telem_console(const char *fmt, ...) { (void)fmt; }
void telem_console_flush(void) {}

#define MINUTES        30
#define DROP_AT_MS     (12.5 * 60000.0)
#define MISS_FROM      12              /* markers missed in minutes MISS_FROM..MISS_TO */
#define MISS_TO        17
#define CHECK_MS       50.0

enum { EV_TICK, EV_BTIME, EV_TMARK, EV_CANDT };
typedef struct { double at; int type; double t, dur; } ev_t;

typedef struct {
    const char *name;
    double drop_ms;
    bool candidates;               /* candidates in the minutes whose marker is missed */
    double burst_from_s, burst_len_s, burst_rate, burst_offset_ms;   /* offset < 0: random */
    int max_off_symbols;           /* bound on symbols emitted >140 ms off */
    int max_admitted_off;          /* frames closed with such symbols before the loss was seen */
    int max_tainted;               /* results computed from a chain holding one */
    double ramp_rate, ramp_len_s;  /* a continuous loss (ms per s) instead of a drop */
} scenario_t;

static ev_t *evs; static int nev, cap;
static void add(double at, int type, double t, double dur) {
    if (nev == cap) { cap = cap ? 2 * cap : 4096; evs = realloc(evs, cap * sizeof *evs); }
    evs[nev++] = (ev_t){ at, type, t, dur };
}
static int cmp_ev(const void *a, const void *b) {
    double x = ((const ev_t *)a)->at, y = ((const ev_t *)b)->at;
    return (x > y) - (x < y);
}

static uint32_t rng = 1;
static double urand(void) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) / 16777216.0; }

static const scenario_t *S;
/* Samples lost by true time t in a ramp: ramp_rate ms each second from DROP_AT_MS */
static double ramp_loss(double t) {
    if (S->ramp_rate <= 0 || t < DROP_AT_MS) return 0.0;
    double d = t - DROP_AT_MS;
    if (d > S->ramp_len_s * 1000.0) d = S->ramp_len_s * 1000.0;
    return d * S->ramp_rate / 1000.0;
}
/* true time -> stream time; < 0 when the samples were lost */
static double to_stream(double t) {
    if (S->ramp_rate > 0) return t - ramp_loss(t);
    if (S->drop_ms <= 0 || t < DROP_AT_MS) return t;
    if (t < DROP_AT_MS + S->drop_ms) return -1.0;
    return t - S->drop_ms;
}
static double to_true(double s) {
    if (S->ramp_rate > 0) {
        double t = s;
        for (int i = 0; i < 8; i++) t = s + ramp_loss(t);
        return t;
    }
    return (S->drop_ms > 0 && s >= DROP_AT_MS) ? s + S->drop_ms : s;
}

static void build(void) {
    nev = 0;
    char frame[61];
    for (int m = 0; m < MINUTES; m++) {
        wwv_td_encode(2026, 324, 14, m, -1, 0, 0, 0, frame);
        for (int sec = 0; sec < 60; sec++) {
            double T = (m * 60.0 + sec) * 1000.0, s;
            if (sec == 0) {
                double lead = T + (urand() - 0.5) * 6.0;
                bool missed = m >= MISS_FROM && m <= MISS_TO;
                if (!missed && (s = to_stream(lead)) >= 0)
                    add(s + 809.0, EV_TMARK, s, 809.0);
                else if (missed && S->candidates && (s = to_stream(lead)) >= 0)
                    add(s + 820.0, EV_CANDT, s, 600.0);
            } else {
                if (sec != 29 && sec != 59 && urand() < 0.9 &&
                    (s = to_stream(T + 15.0 + (urand() - 0.5) * 4.0)) >= 0)
                    add(s, EV_TICK, s, 10.2);
                double w = frame[sec] == 'P' ? 800.0 : frame[sec] == '1' ? 500.0 : 200.0;
                double st = T + 35.0 + (urand() - 0.5) * 16.0, d = w - 5.0 + (urand() - 0.5) * 20.0;
                if (urand() < 0.95 && (s = to_stream(st)) >= 0) add(s + d, EV_BTIME, s, d);
            }
            /* background false ticks, 0.3 a second */
            if (urand() < 0.3 && (s = to_stream(T + urand() * 1000.0)) >= 0) add(s, EV_TICK, s, 10.2);
        }
    }
    if (S->burst_len_s > 0) {
        for (double t = S->burst_from_s * 1000.0; t < (S->burst_from_s + S->burst_len_s) * 1000.0;
             t += 1000.0 / S->burst_rate) {
            double x = S->burst_offset_ms < 0 ? t + urand() * 1000.0
                                              : floor(t / 1000.0) * 1000.0 + 15.0 + S->burst_offset_ms;
            double s = to_stream(x);
            if (s >= 0) add(s, EV_TICK, s, 10.2);
        }
    }
    qsort(evs, nev, sizeof *evs, cmp_ev);
}

/* ---- run ---- */
static wwv_detector_manager_t *mgr;
static double next_check;
static void advance_to(double t) {
    while (next_check < t - 0.01) {
        mgr->now_ms = next_check;
        sync_detector_periodic_check(mgr->sync_detector, (float)next_check);
        bcd_correlator_periodic_check(mgr->bcd_correlator, (float)next_check);
        wwv_time_decoder_periodic(mgr->time_decoder, (float)next_check);
        next_check += CHECK_MS;
    }
    mgr->now_ms = t;
}

/* Held phase error: anchor (stream) against the true :00 mapped to stream time */
static double phase_error_ms(void) {
    double a = sync_detector_get_last_marker_ms(mgr->sync_detector);
    if (a <= 0) return NAN;
    double tr = to_true(a);                      /* the anchor's true time */
    double e = fmod(tr, 60000.0);
    if (e > 30000.0) e -= 60000.0;
    if (e < -30000.0) e += 60000.0;
    return e;
}

static int sym_total, sym_off, tod_right, tod_wrong, tod_conf_right_late, admitted_off;
static int frame_off_seconds;          /* symbols >140 ms off in the open frame */
static int frame_epoch;
static bool tainted;                   /* the chain holds an admitted frame with such symbols */
static int tainted_results;
static double last_tod_conf_at;
static void on_symbol(const bcd_symbol_event_t *e, void *u) {
    (void)u;
    if (e->symbol == BCD_CORR_SYM_NONE) return;
    /* The decoder drops its open frame at a change of epoch: so does the count */
    if (e->phase_epoch != frame_epoch) { frame_off_seconds = 0; frame_epoch = e->phase_epoch; }
    sym_total++;
    /* the second's true start under the phase that binned it */
    double second_start = to_true(e->minute_start_ms + e->second * 1000.0);
    double off = fmod(second_start, 1000.0);
    if (off > 500.0) off -= 1000.0;
    int true_sec = (int)lround(fmod(second_start - off, 60000.0) / 1000.0) % 60;
    if (fabs(off) > 140.0 || true_sec != e->second) { sym_off++; frame_off_seconds++; }
}
static void on_time(const wwv_td_result_t *r, void *u) {
    (void)u;
    double tr = to_true(r->frame_start_ms);
    int m = (int)lround(tr / 60000.0);
    if (r->chain_frames == 1) tainted = false;          /* a new chain */
    if (tainted) tainted_results++;
    if (r->admitted && frame_off_seconds > 0) { admitted_off++; tainted = true; }
    frame_off_seconds = 0;
    if (r->tod_state == WWV_TD_NONE) return;
    bool ok = r->hour == 14 && r->minute == m;
    if (ok) tod_right++; else {
        tod_wrong++;
        printf("     wrong tod %02d:%02d for 14:%02d (%s)\n", r->hour, r->minute, m, wwv_td_state_name(r->tod_state));
    }
    if (ok && r->tod_state == WWV_TD_CONFIRMED && tr > (MISS_TO + 2) * 60000.0) {
        tod_conf_right_late++;
        last_tod_conf_at = tr;
    }
}

static int fails;
static void check(bool ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf("  %-4s ", ok ? "ok" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

static void run(const scenario_t *sc) {
    S = sc; rng = 12345;
    build();
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = NULL; cfg.enable_tone_trackers = false; cfg.enable_correlators = false;
    cfg.enable_slow_marker = false; cfg.verbose = false;
    mgr = wwv_detector_manager_create(&cfg);
    wwv_detector_observer_t obs = {0};
    obs.on_symbol = on_symbol; obs.on_time = on_time;
    wwv_detector_manager_set_observer(mgr, &obs);
    next_check = 0; sym_total = sym_off = tod_right = tod_wrong = tod_conf_right_late = admitted_off = 0;
    frame_off_seconds = 0; frame_epoch = 0; tainted = false; tainted_results = 0;
    double max_err_late = 0;
    for (int i = 0; i < nev; i++) {
        const ev_t *e = &evs[i];
        advance_to(e->at);
        switch (e->type) {
        case EV_TICK: {
            tick_event_t t = { .timestamp_ms = (float)e->t, .duration_ms = (float)e->dur, .peak_energy = 1.0f };
            wwv_routing_on_tick_event(&t, mgr); break; }
        case EV_BTIME: {
            bcd_time_event_t b = { .timestamp_ms = (float)e->t, .duration_ms = (float)e->dur, .peak_energy = 1.0f };
            wwv_routing_on_bcd_time_event(&b, mgr); break; }
        case EV_TMARK: {
            tick_marker_event_t k = { .timestamp_ms = (float)e->at, .start_timestamp_ms = (float)e->t,
                                      .duration_ms = (float)e->dur, .corr_ratio = 1.0f };
            wwv_routing_on_tick_marker_event(&k, mgr); break; }
        case EV_CANDT: {
            tick_marker_candidate_t c = { .start_ms = (float)(e->t + 200.0), .end_ms = (float)e->at,
                                          .lead_from_start_ms = (float)(e->t + 200.0),
                                          .lead_from_end_ms = (float)e->t };
            wwv_routing_on_tick_marker_candidate(&c, mgr); break; }
        }
        /* held-phase error from 3 minutes after the drop (or burst) to the end */
        double settle = (S->drop_ms > 0 || S->ramp_rate > 0) ? (MISS_TO + 1) * 60000.0 + 5000.0
                                        : (S->burst_from_s + S->burst_len_s) * 1000.0 + 3 * 60000.0;
        if (sync_detector_get_state(mgr->sync_detector) == SYNC_LOCKED && to_true(e->at) > settle &&
            (S->drop_ms > 0 || S->burst_len_s > 0 || S->ramp_rate > 0)) {
            double pe = fabs(phase_error_ms());
            if (pe > max_err_late) max_err_late = pe;
        }
    }
    advance_to(to_stream(MINUTES * 60000.0 + 3000.0));
    sync_state_t st = sync_detector_get_state(mgr->sync_detector);
    double pe = phase_error_ms();
    sync_phase_stats_t ps = sync_detector_get_phase_stats(mgr->sync_detector);
    printf("%s: slews=%d jumps=%d snaps=%d candidate_seconds=%d breaks=%d | symbols %d, %d >140 ms off | "
           "tod right %d wrong %d | end %s, phase error %+.1f ms\n", sc->name, ps.slews, ps.jumps, ps.snaps,
           ps.candidate_seconds, ps.breaks, sym_total, sym_off, tod_right, tod_wrong, sync_state_name(st), pe);
    check(tod_wrong == 0, "no time of day decoded wrong (%d)", tod_wrong);
    check(admitted_off <= sc->max_admitted_off,
          "frames admitted holding a symbol >140 ms off its second: %d <= %d", admitted_off, sc->max_admitted_off);
    check(tainted_results <= sc->max_tainted,
          "results decoded from a chain that holds such a frame: %d <= %d", tainted_results, sc->max_tainted);
    check(st == SYNC_LOCKED && fabs(pe) <= 30.0, "LOCKED at the end, phase error %+.1f ms", pe);
    check(max_err_late <= 30.0, "phase within 30 ms once the markers return, or 3 minutes after a burst (max %.1f ms)",
          max_err_late);
    check(tod_conf_right_late > 0, "time of day CONFIRMED right after minute %d (%d frames)", MISS_TO + 2,
          tod_conf_right_late);
    check(sym_off <= sc->max_off_symbols, "symbols >140 ms off: %d <= %d", sym_off, sc->max_off_symbols);
    wwv_detector_manager_destroy(mgr);
}

int main(void) {
    static const scenario_t scs[] = {
        /* name              drop  cand   burst: from len rate offset  bounds: off adm taint */
        { "drop 300 ms",      300, false,        0,  0,  0,     0,          20,  1,  0 },
        { "drop 1000 ms",    1000, true,         0,  0,  0,     0,          40,  1,  0 },
        { "drop 1050 ms",    1050, true,         0,  0,  0,     0,          40,  1,  0 },
        { "drop 1400 ms",    1400, true,         0,  0,  0,     0,          40,  1,  0 },
        /* No candidate: nothing shows whole seconds until the minute-18 marker.
         * The documented limit; only the decoder's frame checks stand between. */
        { "drop 1400 ms, no candidates", 1400, false, 0, 0, 0,  0,        320,  1,  5 },
        { "false ticks, random",       0, false, 600, 40, 4.0, -1.0,       0,  0,  0 },
        { "false ticks, 300 ms late",  0, false, 600, 30, 2.0, 300.0,      0,  0,  0 },
        /* run90g seg2: about 3 ms/s; here 900 ms over 5 minutes, markers missed */
        { "ramp 3 ms/s, 300 s",        0, false,   0,  0,  0,     0,     20,  1,  0, 3.0, 300.0 },
    };
    for (size_t i = 0; i < sizeof scs / sizeof scs[0]; i++) run(&scs[i]);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    free(evs);
    return fails ? 1 : 0;
}
