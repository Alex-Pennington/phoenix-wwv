/* Event replay: runs a recorded air-harness log back through the chain's sync
 * detector, BCD correlator and time decoder.
 *
 *   replay_events harness_log.txt [console.txt] > replay.txt
 *
 * The input is a log written by wwv_chain_log (tests/air/harness_iq or wwv_live
 * --log). Its detector events (TICK, TMARK, MARK, BTIME, BFREQ) are fed, in log
 * order, through the manager's own routing functions, with the manager's 50 ms
 * periodic checks (sync, correlator, decoder) interleaved at their stream times.
 * The detectors do not depend on sync, so the events are what the detectors
 * would report again; what sync, the correlator and the decoder make of them is
 * recomputed by the code under test. The output has the harness's line formats
 * (SYNC, WEV, WCLOSE, SYMCB, SYMMIN, TDRES, TELEM BCDS), so decode_score.py and
 * symbol_second_score.py score it like a harness run.
 *
 * Event times: BTIME and BFREQ carry the manager time they were reported at
 * (at=). TICK, TMARK and MARK are reported at their own timestamp or later; they
 * are delivered at max(previous event time, own timestamp). An event at a
 * periodic-check instant is delivered before that check, as in the manager.
 * The BCD energies are not in the log; they set only a symbol's confidence,
 * which the decoder does not use, so 1.0 is passed.
 *
 * Marker candidates: CANDT and CANDM lines are delivered at their run's end.
 * A log written before they existed has none; given the run's console (the
 * detectors' stdout), the tick path's candidates are rebuilt from its
 * "REJECTED: dur=" gap-zone and marker-interval lines, delivered at their end
 * time. Older consoles print that time to 0.1 s only (so the leads are +-50 ms);
 * consoles with an end= field give it exactly. The marker detector's short runs
 * were not printed before, so they cannot be rebuilt. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "wwv_detector_manager.h"
#include "manager/wwv_detector_manager_internal.h"
#include "wwv_chain_log.h"
#include "telemetry.h"

/* ---- telemetry interposition (as harness_iq) ---- */
bool telem_init(int port) { (void)port; return true; }
void telem_cleanup(void) {}
void telem_enable(uint32_t c) { (void)c; }
void telem_disable(uint32_t c) { (void)c; }
void telem_set_channels(uint32_t c) { (void)c; }
bool telem_is_enabled(telem_channel_t c) { (void)c; return true; }
void telem_send(telem_channel_t c, const char *l) { printf("TELEM ch=%d %s\n", (int)c, l); }
void telem_sendf(telem_channel_t c, const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (c == TELEM_BCDS) printf("TELEM BCDS %s\n", b);
}
void telem_get_stats(uint32_t *s, uint32_t *d) { if (s) *s = 0; if (d) *d = 0; }
void telem_console(const char *fmt, ...) { (void)fmt; }
void telem_console_flush(void) {}

#define CHECK_MS 50.0   /* WWV_PERIODIC_CHECK_SAMPLES at 50 kHz */

static wwv_detector_manager_t *mgr;
static double next_check = 0.0;

/* Run every periodic check strictly before t, as the manager would. */
static void advance_to(double t) {
    while (next_check < t - 0.01) {
        mgr->now_ms = next_check;
        wwv_time_ms_t now = next_check;
        if (mgr->sync_detector) sync_detector_periodic_check(mgr->sync_detector, now);
        if (mgr->bcd_correlator) bcd_correlator_periodic_check(mgr->bcd_correlator, now);
        if (mgr->time_decoder) wwv_time_decoder_periodic(mgr->time_decoder, now);
        next_check += CHECK_MS;
    }
    mgr->now_ms = t;
}

/* Tick-path candidates rebuilt from a console */
typedef struct { double end, dur; } cand_t;
static cand_t *cands; static int n_cands, next_cand;

static void deliver_candidates_before(double t, double *now) {
    while (next_cand < n_cands && cands[next_cand].end <= t) {
        cand_t c = cands[next_cand++];
        if (c.end > *now) *now = c.end;
        advance_to(*now);
        const double clean = MARKER_ACTUAL_DURATION_MS + 2.0 * TICK_FFT_SIZE * 1000.0 / TICK_SAMPLE_RATE;
        tick_marker_candidate_t e = { .start_ms = c.end - c.dur, .end_ms = c.end,
                                      .lead_from_start_ms = c.end - c.dur - TICK_FILTER_DELAY_MS,
                                      .lead_from_end_ms = c.end - clean - TICK_FILTER_DELAY_MS };
        wwv_routing_on_tick_marker_candidate(&e, mgr);
    }
}

static int cmp_cand(const void *a, const void *b) {
    double x = ((const cand_t *)a)->end, y = ((const cand_t *)b)->end;
    return (x > y) - (x < y);
}

/* Gap-zone and marker-interval rejections of at least TICK_CANDIDATE_MIN_MS, as
 * the tick detector would report them. */
static void load_console(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) { perror(path); exit(2); }
    char line[1024]; int cap = 0;
    while (fgets(line, sizeof line, fp)) {
        double s, d;
        if (sscanf(line, "[ %lfs] REJECTED: dur=%lfms", &s, &d) != 2) continue;
        bool gap = strstr(line, "gap zone") != NULL, interval = strstr(line, "marker-like") != NULL;
        if (!interval && !(gap && d >= TICK_CANDIDATE_MIN_MS)) continue;
        const char *e = strstr(line, " end=");
        double end = e ? strtod(e + 5, NULL) : s * 1000.0;
        if (n_cands == cap) { cap = cap ? 2 * cap : 256; cands = realloc(cands, cap * sizeof *cands); }
        cands[n_cands++] = (cand_t){ end, d };
    }
    fclose(fp);
    qsort(cands, n_cands, sizeof *cands, cmp_cand);
}

static bool field(const char *line, const char *key, double *v) {
    char pat[32]; snprintf(pat, sizeof pat, " %s=", key);
    const char *p = strstr(line, pat);
    if (!p) return false;
    *v = strtod(p + strlen(pat), NULL);
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s harness_log.txt [console.txt]\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "r");
    if (!fp) { perror(argv[1]); return 2; }
    /* A log with CANDT lines carries its own candidates */
    bool has_cand_lines = false;
    {
        char l[1024];
        while (fgets(l, sizeof l, fp)) if (strncmp(l, "CANDT ", 6) == 0) { has_cand_lines = true; break; }
        rewind(fp);
    }
    if (argc > 2 && !has_cand_lines) load_console(argv[2]);
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = NULL;
    cfg.enable_tone_trackers = false;
    cfg.enable_correlators = false;
    cfg.enable_slow_marker = false;
    cfg.verbose = false;
    mgr = wwv_detector_manager_create(&cfg);
    if (!mgr) { fprintf(stderr, "detector manager create failed\n"); return 2; }
    wwv_chain_log_t *log = wwv_chain_log_create(stdout, mgr);
    const wwv_detector_observer_t obs = wwv_chain_log_observer(log);
    wwv_detector_manager_set_observer(mgr, &obs);
    printf("CONFIG REPLAY=%s%s%s candidates=%d\n", argv[1], n_cands ? " CONSOLE=" : "",
           n_cands ? argv[2] : "", n_cands);

    char line[1024];
    double now = 0.0, end_ms = 0.0;
    int n_tick = 0, n_tmark = 0, n_mark = 0, n_bt = 0, n_bf = 0, n_tmarkw = 0, n_markw = 0;
    while (fgets(line, sizeof line, fp)) {
        double t, d, a, l;
        if (strncmp(line, "TICK ", 5) == 0 && field(line, "t", &t) && field(line, "dur", &d)) {
            double in = 0.0; field(line, "int", &in);
            double n = 0.0; field(line, "n", &n);
            deliver_candidates_before(t, &now);
            if (t > now) now = t;
            advance_to(now);
            tick_event_t e = { .tick_number = (int)n, .timestamp_ms = t,
                               .interval_ms = (float)in, .duration_ms = (float)d, .peak_energy = 1.0f };
            wwv_routing_on_tick_event(&e, mgr);
            n_tick++;
        } else if (strncmp(line, "TMARK ", 6) == 0 && field(line, "trail", &t) &&
                   field(line, "lead", &l) && field(line, "dur", &d)) {
            double n = 0.0; field(line, "n", &n);
            deliver_candidates_before(t, &now);
            if (t > now) now = t;
            advance_to(now);
            tick_marker_event_t e = { .marker_number = (int)n, .timestamp_ms = t,
                                      .start_timestamp_ms = l, .duration_ms = (float)d,
                                      .corr_ratio = 1.0f };
            wwv_routing_on_tick_marker_event(&e, mgr);
            n_tmark++;
        } else if (strncmp(line, "MARK ", 5) == 0 && field(line, "t", &t) &&
                   field(line, "lead", &l) && field(line, "dur", &d)) {
            double n = 0.0; field(line, "n", &n);
            deliver_candidates_before(t, &now);
            if (t > now) now = t;
            advance_to(now);
            marker_event_t e = { .marker_number = (int)n, .timestamp_ms = t,
                                 .accumulated_energy = 1.0f, .peak_energy = 1.0f,
                                 .duration_ms = (float)d, .start_timestamp_ms = l };
            wwv_routing_on_marker_event(&e, mgr);
            n_mark++;
        } else if (strncmp(line, "BTIME ", 6) == 0 && field(line, "t", &t) &&
                   field(line, "dur", &d) && field(line, "at", &a)) {
            deliver_candidates_before(a, &now);
            if (a > now) now = a;
            advance_to(now);
            bcd_time_event_t e = { .timestamp_ms = t, .duration_ms = (float)d, .peak_energy = 1.0f };
            wwv_routing_on_bcd_time_event(&e, mgr);
            n_bt++;
        } else if (strncmp(line, "BFREQ ", 6) == 0 && field(line, "t", &t) &&
                   field(line, "dur", &d) && field(line, "at", &a)) {
            deliver_candidates_before(a, &now);
            if (a > now) now = a;
            advance_to(now);
            bcd_freq_event_t e = { .timestamp_ms = t, .duration_ms = (float)d, .accumulated_energy = 1.0f };
            wwv_routing_on_bcd_freq_event(&e, mgr);
            n_bf++;
        } else if (strncmp(line, "CANDT ", 6) == 0 && field(line, "start", &l) && field(line, "end", &t)) {
            double ls = 0.0, le = 0.0; field(line, "lead_s", &ls); field(line, "lead_e", &le);
            if (t > now) now = t;
            advance_to(now);
            tick_marker_candidate_t e = { .start_ms = l, .end_ms = t,
                                          .lead_from_start_ms = ls, .lead_from_end_ms = le };
            wwv_routing_on_tick_marker_candidate(&e, mgr);
            n_tmarkw++;
        } else if (strncmp(line, "CANDM ", 6) == 0 && field(line, "t", &t) &&
                   field(line, "lead", &l) && field(line, "dur", &d)) {
            if (t > now) now = t;
            advance_to(now);
            marker_event_t e = { .timestamp_ms = t, .accumulated_energy = 1.0f, .peak_energy = 1.0f,
                                 .duration_ms = (float)d, .start_timestamp_ms = l };
            wwv_routing_on_marker_candidate(&e, mgr);
            n_markw++;
        } else if (strncmp(line, "GAP ", 4) == 0 && field(line, "start", &t) && field(line, "len", &d)) {
            /* a filled gap: marked where wwv_live marked it, before the span's events */
            wwv_detector_manager_mark_gap(mgr, t, d);
        } else if (strncmp(line, "END ", 4) == 0) {
            double s;
            if (field(line, "samples", &s)) end_ms = s / 50.0;   /* 50 kHz */
        }
    }
    fclose(fp);
    deliver_candidates_before(end_ms > now ? end_ms : now, &now);
    if (end_ms > now) advance_to(end_ms);
    sync_detector_t *sd = mgr->sync_detector;
    frame_time_t ft = sync_detector_get_frame_time(sd);
    printf("END replay sync=%s conf=%.2f sec=%d ticks=%d tmarks=%d markers=%d btime=%d bfreq=%d "
           "candidates tick=%d marker=%d console=%d\n",
           sync_state_name(sync_detector_get_state(sd)), sync_detector_get_confidence(sd),
           ft.current_second, n_tick, n_tmark, n_mark, n_bt, n_bf, n_tmarkw, n_markw, n_cands);
    sync_phase_stats_t ps = sync_detector_get_phase_stats(sd);
    printf("PHASE slews=%d slewed_ms=%.0f jumps=%d snaps=%d ignored=%d candidate_seconds=%d breaks=%d epoch=%d\n",
           ps.slews, ps.slewed_ms, ps.jumps, ps.snaps, ps.jumps_ignored, ps.candidate_seconds, ps.breaks,
           sync_detector_get_phase_epoch(sd));
    wwv_gap_drops_t gd = wwv_detector_manager_get_gap_drops(mgr);
    if (gd.tick || gd.tmark || gd.marker || gd.candidate || gd.btime || gd.bfreq || mgr->n_gaps)
        printf("GAPDROPS tick=%d tmark=%d mark=%d cand=%d btime=%d bfreq=%d\n",
               gd.tick, gd.tmark, gd.marker, gd.candidate, gd.btime, gd.bfreq);
    wwv_detector_manager_destroy(mgr);
    wwv_chain_log_destroy(log);
    return 0;
}
