/* Air-capture harness. Copied from wwv-capture/harness/harness_iq_{,norm_}head.c
 * (a file-input variant of the audit harness) and merged into one program:
 *   harness_iq [-n] iq_f32_50k.raw
 * -n applies the waterfall.c slow AGC before the channel filters (the "norm" wiring).
 * Output lines are identical to the originals so wwv-capture/score.py scores them.
 *
 * Reads interleaved float32 I/Q at exactly 50 kHz (already scaled the way
 * waterfall.c scales int16: /32768) from argv[1] and runs it through the library's
 * own wiring, wwv_detector_manager (channel filters -> tick/marker/BCD time/freq
 * detectors -> sync detector -> BCD correlator -> time decoder). The harness only
 * observes: every line below is printed from the manager's observer, so the chain
 * scored here is the chain every host runs.
 * Timestamps printed are the detectors' own (ms since sample 0). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "wwv_detector_manager.h"
#include "telemetry.h"
#include "bcd_correlator_internal.h"  /* WTRACE: window accumulators */

#define FS 50000.0
static int PRINT_TELEM = 1;

/* ---- telemetry interposition ---- */
bool telem_init(int port) { (void)port; return true; }
void telem_cleanup(void) {}
void telem_enable(uint32_t c) { (void)c; }
void telem_disable(uint32_t c) { (void)c; }
void telem_set_channels(uint32_t c) { (void)c; }
bool telem_is_enabled(telem_channel_t c) { (void)c; return true; }
void telem_send(telem_channel_t c, const char *l) { if (PRINT_TELEM) printf("TELEM ch=%d %s\n", (int)c, l); }
void telem_sendf(telem_channel_t c, const char *fmt, ...) {
    if (!PRINT_TELEM) return;
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (c == TELEM_BCDS) printf("TELEM BCDS %s\n", b);
}
void telem_get_stats(uint32_t *s, uint32_t *d) { if (s) *s = 0; if (d) *d = 0; }
void telem_console(const char *fmt, ...) { (void)fmt; }
void telem_console_flush(void) {}

static wwv_detector_manager_t *g_mgr;
static tick_detector_t *g_tick; static marker_detector_t *g_mark;
static sync_detector_t *g_sync; static bcd_correlator_t *g_bcd;

static void on_tick(const tick_event_t *e, void *u) { (void)u;
    printf("TICK n=%d t=%.1f dur=%.1f int=%.1f\n", e->tick_number, e->timestamp_ms, e->duration_ms, e->interval_ms); }
static void on_tick_marker(const tick_marker_event_t *e, void *u) { (void)u;
    printf("TMARK n=%d trail=%.1f lead=%.1f dur=%.1f\n", e->marker_number, e->timestamp_ms, e->start_timestamp_ms, e->duration_ms); }
static void on_marker(const marker_event_t *e, void *u) { (void)u;
    printf("MARK n=%d t=%.1f dur=%.1f lead=%.1f\n", e->marker_number, e->timestamp_ms, e->duration_ms, e->start_timestamp_ms); }
/* WTRACE: which window each BCD event landed in, and what each window held when it
 * closed (read from the correlator's accumulators inside the symbol callback, before
 * they are reset). Scored by tests/air/bcd_window_trace.py. */
static void trace_event(char src, float t, float dur, bcd_correlator_t *c) {
    if (c->window_open)
        printf("WEV src=%c t=%.1f dur=%.1f win_sec=%d win_start=%.1f anchor=%.1f\n",
               src, t, dur, c->current_second, c->window_start_ms, c->window_anchor_ms);
    else printf("WEV src=%c t=%.1f dur=%.1f win_sec=-1\n", src, t, dur);
}
static void on_bt(const bcd_time_event_t *e, void *u) { (void)u;
    printf("BTIME t=%.1f dur=%.1f at=%.1f\n", e->timestamp_ms, e->duration_ms, wwv_detector_manager_get_now_ms(g_mgr)); }
static void on_bt_routed(const bcd_time_event_t *e, void *u) { (void)u;
    trace_event('T', e->timestamp_ms, e->duration_ms, g_bcd); }
static void on_bf(const bcd_freq_event_t *e, void *u) { (void)u;
    printf("BFREQ t=%.1f dur=%.1f at=%.1f\n", e->timestamp_ms, e->duration_ms, wwv_detector_manager_get_now_ms(g_mgr)); }
static void on_bf_routed(const bcd_freq_event_t *e, void *u) { (void)u;
    trace_event('F', e->timestamp_ms, e->duration_ms, g_bcd); }
static void on_sym(const bcd_symbol_event_t *e, void *u) { (void)u;
    printf("WCLOSE sec=%d win_start=%.1f anchor=%.1f tn=%d tfirst=%.1f tlast=%.1f tdur=%.1f fn=%d ffirst=%.1f flast=%.1f fdur=%.1f est=%.1f\n",
           g_bcd->current_second, g_bcd->window_start_ms, g_bcd->window_anchor_ms,
           g_bcd->time_event_count, g_bcd->time_first_ms, g_bcd->time_last_ms, g_bcd->time_duration_sum,
           g_bcd->freq_event_count, g_bcd->freq_first_ms, g_bcd->freq_last_ms, g_bcd->freq_duration_sum,
           e->duration_ms);
    printf("SYMCB sym=%c t=%.1f dur=%.1f conf=%.2f src=%s sec=%d\n", bcd_corr_symbol_char(e->symbol), e->timestamp_ms, e->duration_ms, e->confidence, e->source, e->second);
    printf("SYMMIN sec=%d minute_start=%.1f pulse_offset=%.1f\n", e->second, e->minute_start_ms, e->pulse_offset_ms); }
static void on_time(const wwv_td_result_t *r, void *u) { (void)u;
    printf("TDRES start=%.1f at=%.1f\n", r->frame_start_ms, wwv_detector_manager_get_now_ms(g_mgr)); }
static void on_state(sync_state_t o, sync_state_t n, float c, void *u) { (void)u;
    printf("SYNC %s->%s conf=%.2f\n", sync_state_name(o), sync_state_name(n), c); }

int main(int argc, char **argv) {
    int norm = 0;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { norm = 1; argv++; argc--; }
    if (argc < 2) { fprintf(stderr, "usage: %s [-n] iq_f32_50k.raw\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = NULL;
    cfg.enable_tone_trackers = false;
    cfg.enable_correlators = false;
    cfg.enable_slow_marker = false;
    cfg.enable_agc = norm;
    cfg.verbose = false;
    g_mgr = wwv_detector_manager_create(&cfg);
    if (!g_mgr) { fprintf(stderr, "detector manager create failed\n"); return 2; }
    const wwv_detector_observer_t obs = {
        .on_tick = on_tick, .on_tick_marker = on_tick_marker, .on_marker = on_marker,
        .on_bcd_time = on_bt, .on_bcd_time_routed = on_bt_routed,
        .on_bcd_freq = on_bf, .on_bcd_freq_routed = on_bf_routed,
        .on_symbol = on_sym, .on_time = on_time, .on_sync_state = on_state };
    wwv_detector_manager_set_observer(g_mgr, &obs);
    g_tick = wwv_detector_manager_get_tick_detector(g_mgr);
    g_mark = wwv_detector_manager_get_marker_detector(g_mgr);
    g_sync = wwv_detector_manager_get_sync_detector(g_mgr);
    g_bcd = wwv_detector_manager_get_bcd_correlator(g_mgr);
    if (norm) printf("CONFIG NORM=1 FILE=%s FS=%.0f\n", argv[1], FS);
    else printf("CONFIG FILE=%s FS=%.0f\n", argv[1], FS);
    static float buf[2 * 8192];
    long n = 0; size_t got;
    while ((got = fread(buf, sizeof(float) * 2, 8192, fp)) > 0) {
        for (size_t k = 0; k < got; k++, n++)
            wwv_detector_manager_process_detector_sample(g_mgr, buf[2 * k], buf[2 * k + 1]);
    }
    fclose(fp);
    frame_time_t ft = sync_detector_get_frame_time(g_sync);
    printf("END samples=%ld sync=%s conf=%.2f sec=%d ticks=%d markers=%d bcdsyms=%d\n", n, sync_state_name(sync_detector_get_state(g_sync)),
           sync_detector_get_confidence(g_sync), ft.current_second, tick_detector_get_tick_count(g_tick),
           marker_detector_get_marker_count(g_mark), bcd_correlator_get_symbol_count(g_bcd));
    printf("BCDWIN silent=%d late_events=%d\n", g_bcd->silent_windows, g_bcd->late_events);
    PRINT_TELEM = 1;
    wwv_detector_manager_destroy(g_mgr);
    return 0;
}
