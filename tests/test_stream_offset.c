/* Stream time offset: sync, the BCD correlator and the time decoder give the same
 * result 40 h into a run as at its start.
 *
 * One synthetic signal is fed twice through the manager's own routing (as
 * tests/air/replay_events.c does), once from stream time 0 and once from 40 h,
 * with the manager's 50 ms periodic checks interleaved. Each run writes a
 * transcript, with every time relative to its offset: sync state changes, every
 * BCD symbol (second, symbol, pulse offset, minute start), every time-decoder
 * result (frame, part states and values, frame start, when it was reported) and
 * the phase tracker's counts. The two transcripts must be identical.
 *
 * The signal: 20 minutes of WWV in a time code that runs 1/16 ms slow per second
 * (62.5 ppm, so the phase tracker slews), with a deterministic jitter of up to
 * +-2 ms on every edge. Ticks each second but :00, :29 and :59, a tick-path and a
 * marker-detector marker at :00, and a BTIME pulse each second of the encoded
 * minute (200/500/800 ms). Markers are left out for minutes 8-13, so for six
 * minutes the phase is held by edges alone (holdover). Every time is a multiple
 * of 1/64 ms, so offset + time is exact in double and the runs are comparable
 * exactly. A float stream time steps 16 ms at 40 h.
 *
 * The test uses double rather than wwv_time_ms_t, so it also builds against a
 * chain with float stream times, where it fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "wwv_detector_manager.h"
#include "manager/wwv_detector_manager_internal.h"
#include "telemetry.h"

/* ---- telemetry stubs (as replay_events) ---- */
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

#define OFFSET_40H_MS (40.0 * 3600.0 * 1000.0)
#define MINUTES       20
#define T0_MS         10000.0          /* first minute's start, from the offset */
#define DRIFT_MS      (1.0 / 16.0)     /* per second */
#define CHECK_MS      50.0             /* WWV_PERIODIC_CHECK_SAMPLES at 50 kHz */

typedef enum { EV_TICK, EV_TMARK, EV_MARK, EV_BTIME } ev_kind_t;
typedef struct { double at, t, dur; ev_kind_t kind; } ev_t;

static ev_t evs[MINUTES * 128];   /* per minute: 57 ticks, 60 BTIME, 2 markers */
static int n_evs;

/* ---- transcript ---- */
static char *tr; static size_t tr_len, tr_cap;
static double g_off;
static wwv_detector_manager_t *mgr;

static void say(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (tr_len + (size_t)n + 1 > tr_cap) { tr_cap = 2 * (tr_cap + (size_t)n + 1); tr = realloc(tr, tr_cap); }
    memcpy(tr + tr_len, b, (size_t)n + 1); tr_len += (size_t)n;
}
static double rel(double t) { return t - g_off; }

static void on_symbol(const bcd_symbol_event_t *e, void *u) {
    (void)u;
    say("SYM t=%.4f sec=%d sym=%c src=%s off=%.4f min=%.4f epoch=%d\n", rel(e->timestamp_ms), e->second,
        bcd_corr_symbol_char(e->symbol), e->source, e->pulse_offset_ms, rel(e->minute_start_ms),
        e->phase_epoch);
}
static const char *st(wwv_td_state_t s) { return s == WWV_TD_CONFIRMED ? "C" : s == WWV_TD_DECODED ? "D" : s == WWV_TD_PREDICTED ? "P" : "-"; }
static int n_tod_confirmed, n_date_confirmed;
static void on_time(const wwv_td_result_t *r, void *u) {
    (void)u;
    if (r->tod_state == WWV_TD_CONFIRMED) n_tod_confirmed++;
    if (r->date_state == WWV_TD_CONFIRMED) n_date_confirmed++;
    say("TD start=%.4f at=%.4f chain=%d frame=%s tod=%s %02d:%02d date=%s %d/%d dut1=%s%d obs=%d\n",
        rel(r->frame_start_ms), rel(mgr->now_ms), r->chain_minute, r->frame, st(r->tod_state), r->hour,
        r->minute, st(r->date_state), r->year, r->doy, st(r->dut1_state), r->dut1_tenths, r->observations);
}
static void on_sync(sync_state_t o, sync_state_t n, float c, void *u) {
    (void)u;
    say("SYNC at=%.4f %s->%s conf=%.3f\n", rel(mgr->now_ms), sync_state_name(o), sync_state_name(n), c);
}

/* ---- the signal ---- */
static uint32_t lcg;
static double jitter(void) {              /* -2..+2 ms in 1/64 ms steps */
    lcg = lcg * 1664525u + 1013904223u;
    return (double)((int)((lcg >> 16) % 257) - 128) / 64.0;
}
static void add(ev_kind_t k, double t, double dur, double at) {
    evs[n_evs++] = (ev_t){ at, t, dur, k };
}
static int cmp_ev(const void *a, const void *b) {
    const ev_t *x = a, *y = b;
    return (x->at > y->at) - (x->at < y->at);
}
static void build_signal(void) {
    n_evs = 0; lcg = 12345u;
    for (int k = 0; k < MINUTES; k++) {
        char f[61];
        wwv_td_encode(2026, 325, 10, 7 + k, 3, 0, 0, 0, f);
        for (int s = 0; s < 60; s++) {
            int abs_s = 60 * k + s;
            double sec = T0_MS + 1000.0 * abs_s + DRIFT_MS * abs_s;   /* the true second */
            bool marker_minute = k < 8 || k > 13;
            if (s == 0 && marker_minute) {
                double lead = sec + jitter();
                add(EV_TMARK, lead, 800.0, lead + 800.0 + 3.0);
                add(EV_MARK, lead + 1500.0, 800.0, lead + 1500.0);
            }
            if (s != 0 && s != 29 && s != 59) {
                double t = sec + 15.0 + jitter();
                add(EV_TICK, t, 5.0, t);
            }
            double w = f[s] == 'P' ? 800.0 : f[s] == '1' ? 500.0 : 200.0;
            double t = sec + 30.0 + jitter();
            add(EV_BTIME, t, w, t + w);
        }
    }
    qsort(evs, (size_t)n_evs, sizeof *evs, cmp_ev);
}

/* ---- one run ---- */
static double next_check;
static void advance_to(double t) {
    while (next_check < t - 0.01) {
        mgr->now_ms = next_check;
        if (mgr->sync_detector) sync_detector_periodic_check(mgr->sync_detector, next_check);
        if (mgr->bcd_correlator) bcd_correlator_periodic_check(mgr->bcd_correlator, next_check);
        if (mgr->time_decoder) wwv_time_decoder_periodic(mgr->time_decoder, next_check);
        next_check += CHECK_MS;
    }
    mgr->now_ms = t;
}

static char *run(double off) {
    tr = NULL; tr_len = tr_cap = 0; g_off = off; next_check = off;
    n_tod_confirmed = n_date_confirmed = 0;
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = NULL;
    cfg.enable_tone_trackers = false;
    cfg.enable_correlators = false;
    cfg.enable_slow_marker = false;
    cfg.verbose = false;
    mgr = wwv_detector_manager_create(&cfg);
    if (!mgr) { fprintf(stderr, "detector manager create failed\n"); exit(2); }
    wwv_detector_observer_t obs = { 0 };
    obs.on_symbol = on_symbol;
    obs.on_time = on_time;
    obs.on_sync_state = on_sync;
    wwv_detector_manager_set_observer(mgr, &obs);

    int nt = 0;
    for (int i = 0; i < n_evs; i++) {
        const ev_t *e = &evs[i];
        double at = off + e->at, t = off + e->t;
        advance_to(at);
        switch (e->kind) {
        case EV_TICK: {
            tick_event_t te = { .tick_number = ++nt, .timestamp_ms = t, .duration_ms = (float)e->dur,
                                .peak_energy = 1.0f };
            wwv_routing_on_tick_event(&te, mgr);
            break; }
        case EV_TMARK: {
            tick_marker_event_t me = { .timestamp_ms = t + e->dur, .start_timestamp_ms = t,
                                       .duration_ms = (float)e->dur, .corr_ratio = 5.0f };
            wwv_routing_on_tick_marker_event(&me, mgr);
            break; }
        case EV_MARK: {
            marker_event_t me = { .timestamp_ms = t, .start_timestamp_ms = t - 1500.0,
                                  .duration_ms = (float)e->dur, .accumulated_energy = 1.0f,
                                  .peak_energy = 1.0f };
            wwv_routing_on_marker_event(&me, mgr);
            break; }
        case EV_BTIME: {
            bcd_time_event_t be = { .timestamp_ms = t, .duration_ms = (float)e->dur, .peak_energy = 1.0f };
            wwv_routing_on_bcd_time_event(&be, mgr);
            break; }
        }
    }
    advance_to(off + evs[n_evs - 1].at + 65000.0);

    sync_detector_t *sd = mgr->sync_detector;
    sync_phase_stats_t ps = sync_detector_get_phase_stats(sd);
    say("END sync=%s anchor=%.4f slews=%d slewed=%.4f jumps=%d breaks=%d epoch=%d resets=%d\n",
        sync_state_name(sync_detector_get_state(sd)), rel(sync_detector_get_last_marker_ms(sd)),
        ps.slews, ps.slewed_ms, ps.jumps, ps.breaks, sync_detector_get_phase_epoch(sd),
        sync_detector_get_phase_resets(sd));
    wwv_detector_manager_destroy(mgr);
    mgr = NULL;
    return tr;
}

static int failures;
static void check(bool ok, const char *what) {
    fprintf(stderr, "%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

int main(void) {
    /* The chain's own console output is not the result: keep it out of the way */
    if (!freopen("/dev/null", "w", stdout)) return 2;
    build_signal();

    char *a = run(0.0);
    int tod0 = n_tod_confirmed, date0 = n_date_confirmed;
    char *b = run(OFFSET_40H_MS);

    int nsym = 0, nslew = 0;
    for (const char *p = a; (p = strstr(p, "SYM ")); p++) nsym++;
    const char *end = strstr(a, "END ");
    if (end) sscanf(strstr(end, "slews="), "slews=%d", &nslew);

    fprintf(stderr, "at 0 h: %d symbols, %d slews, %d results with TOD CONFIRMED, %d with date CONFIRMED\n",
            nsym, nslew, tod0, date0);
    fprintf(stderr, "%s", end ? end : "no END line\n");
    check(nsym >= MINUTES * 60 - 120, "the run at 0 h emits a symbol for nearly every second");
    check(nslew > 0, "the phase tracker slews (the time code drifts)");
    check(tod0 > 0 && date0 > 0, "the run at 0 h reaches TOD and date CONFIRMED");
    check(end && strstr(end, "sync=LOCKED"), "sync ends LOCKED");

    bool same = strcmp(a, b) == 0;
    check(same, "the transcript at 40 h is identical to the one at 0 h");
    if (!same) {
        const char *p = a, *q = b; int line = 1;
        while (*p && *p == *q) { if (*p == '\n') line++; p++; q++; }
        while (p > a && p[-1] != '\n') { p--; q--; }
        fprintf(stderr, "first difference, line %d:\n   0 h: %.*s\n  40 h: %.*s\n", line,
                (int)strcspn(p, "\n"), p, (int)strcspn(q, "\n"), q);
    }
    free(a); free(b);
    return failures ? 1 : 0;
}
