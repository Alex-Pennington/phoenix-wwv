/* Air-capture harness. Copied from wwv-capture/harness/harness_iq_{,norm_}head.c
 * (a file-input variant of the audit harness) and merged into one program:
 *   harness_iq [-n] iq_f32_50k.raw
 * -n applies the waterfall.c slow AGC before the channel filters (the "norm" wiring).
 * Output lines are identical to the originals so wwv-capture/score.py scores them.
 *
 * Reads interleaved float32 I/Q at exactly 50 kHz (already scaled the way
 * waterfall.c scales int16: /32768) from argv[1] and runs it through the same
 * chain as the audit harness (channel filters -> tick/marker/BCD time/freq
 * detectors -> sync detector -> BCD correlator), wired as waterfall.c (1ec38d1).
 * Timestamps printed are the detectors' own (ms since sample 0).
 * NORM (-n): applies waterfall.c 1ec38d1 normalize() (slow AGC, lines 252-258,
 * 367-374, applied at 2185-2187) before the channel filters, as the real path did. */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "channel_filters.h"
#include "tick_detector.h"
#include "marker_detector.h"
#include "bcd_time_detector.h"
#include "bcd_freq_detector.h"
#include "bcd_correlator.h"
#include "sync_detector.h"
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

typedef struct { float level; int warmup; } normalizer_t;
static normalizer_t g_normalizer = {0.01f, 0};
static float normalize(normalizer_t *n, float i, float q) {
    float mag = sqrtf(i*i + q*q);
    float alpha = (n->warmup < 50000) ? 0.01f : 0.0001f;  /* Fast warmup, then slow */
    n->level += alpha * (mag - n->level);
    n->warmup++;
    if (n->level < 0.0001f) n->level = 0.0001f;
    return 1.0f / n->level;
}
static tick_detector_t *g_tick; static marker_detector_t *g_mark;
static sync_detector_t *g_sync; static bcd_correlator_t *g_bcd;

static void on_tick(const tick_event_t *e, void *u) { (void)u;
    printf("TICK n=%d t=%.1f dur=%.1f int=%.1f\n", e->tick_number, e->timestamp_ms, e->duration_ms, e->interval_ms); }
static void on_tick_marker(const tick_marker_event_t *e, void *u) { (void)u;
    printf("TMARK n=%d trail=%.1f lead=%.1f dur=%.1f\n", e->marker_number, e->timestamp_ms, e->start_timestamp_ms, e->duration_ms);
    sync_detector_tick_marker(g_sync, e->start_timestamp_ms, e->duration_ms, e->corr_ratio);
    if (tick_detector_get_epoch_source(g_tick) == EPOCH_SOURCE_NONE)
        tick_detector_set_epoch_with_source(g_tick, e->start_timestamp_ms, EPOCH_SOURCE_MARKER, 0.7f);
}
static void on_marker(const marker_event_t *e, void *u) { (void)u;
    printf("MARK n=%d t=%.1f dur=%.1f lead=%.1f\n", e->marker_number, e->timestamp_ms, e->duration_ms, e->start_timestamp_ms);
    sync_detector_marker_event(g_sync, e->timestamp_ms, e->accumulated_energy, e->duration_ms);
}
/* WTRACE: which window each BCD event landed in, and what each window held when it
 * closed (read from the correlator's accumulators inside the symbol callback, before
 * they are reset). Scored by tests/air/bcd_window_trace.py. */
static void trace_event(char src, float t, float dur, bcd_correlator_t *c) {
    if (c->window_open)
        printf("WEV src=%c t=%.1f dur=%.1f win_sec=%d win_start=%.1f anchor=%.1f\n",
               src, t, dur, c->current_second, c->window_start_ms, c->window_anchor_ms);
    else printf("WEV src=%c t=%.1f dur=%.1f win_sec=-1\n", src, t, dur);
}
static void on_bt(const bcd_time_event_t *e, void *u) {
    printf("BTIME t=%.1f dur=%.1f\n", e->timestamp_ms, e->duration_ms);
    bcd_correlator_time_event((bcd_correlator_t *)u, e->timestamp_ms, e->duration_ms, e->peak_energy);
    trace_event('T', e->timestamp_ms, e->duration_ms, (bcd_correlator_t *)u); }
static void on_bf(const bcd_freq_event_t *e, void *u) {
    printf("BFREQ t=%.1f dur=%.1f\n", e->timestamp_ms, e->duration_ms);
    bcd_correlator_freq_event((bcd_correlator_t *)u, e->timestamp_ms, e->duration_ms, e->accumulated_energy);
    trace_event('F', e->timestamp_ms, e->duration_ms, (bcd_correlator_t *)u); }
static void on_sym(const bcd_symbol_event_t *e, void *u) { (void)u;
    printf("WCLOSE sec=%d win_start=%.1f anchor=%.1f tn=%d tfirst=%.1f tlast=%.1f tdur=%.1f fn=%d ffirst=%.1f flast=%.1f fdur=%.1f est=%.1f\n",
           g_bcd->current_second, g_bcd->window_start_ms, g_bcd->window_anchor_ms,
           g_bcd->time_event_count, g_bcd->time_first_ms, g_bcd->time_last_ms, g_bcd->time_duration_sum,
           g_bcd->freq_event_count, g_bcd->freq_first_ms, g_bcd->freq_last_ms, g_bcd->freq_duration_sum,
           e->duration_ms);
    printf("SYMCB sym=%c t=%.1f dur=%.1f conf=%.2f src=%s sec=%d\n", bcd_corr_symbol_char(e->symbol), e->timestamp_ms, e->duration_ms, e->confidence, e->source, e->second); }
static void on_state(sync_state_t o, sync_state_t n, float c, void *u) { (void)u;
    printf("SYNC %s->%s conf=%.2f\n", sync_state_name(o), sync_state_name(n), c); }

int main(int argc, char **argv) {
    int norm = 0;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { norm = 1; argv++; argc--; }
    if (argc < 2) { fprintf(stderr, "usage: %s [-n] iq_f32_50k.raw\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    g_tick = tick_detector_create(NULL); g_mark = marker_detector_create(NULL);
    g_sync = sync_detector_create(NULL); g_bcd = bcd_correlator_create(NULL);
    bcd_time_detector_t *bt = bcd_time_detector_create(NULL);
    bcd_freq_detector_t *bf = bcd_freq_detector_create(NULL);
    tick_detector_set_callback(g_tick, on_tick, NULL);
    tick_detector_set_marker_callback(g_tick, on_tick_marker, NULL);
    marker_detector_set_callback(g_mark, on_marker, NULL);
    bcd_time_detector_set_callback(bt, on_bt, g_bcd);
    bcd_freq_detector_set_callback(bf, on_bf, g_bcd);
    bcd_correlator_set_sync_source(g_bcd, g_sync);
    bcd_correlator_set_callback(g_bcd, on_sym, NULL);
    sync_detector_set_state_callback(g_sync, on_state, NULL);
    sync_channel_t si, sq; data_channel_t di, dq;
    sync_channel_init(&si); sync_channel_init(&sq); data_channel_init(&di); data_channel_init(&dq);
    if (norm) printf("CONFIG NORM=1 FILE=%s FS=%.0f\n", argv[1], FS);
    else printf("CONFIG FILE=%s FS=%.0f\n", argv[1], FS);
    static float buf[2 * 8192];
    long n = 0; size_t got;
    while ((got = fread(buf, sizeof(float) * 2, 8192, fp)) > 0) {
        for (size_t k = 0; k < got; k++, n++) {
            float I = buf[2 * k], Q = buf[2 * k + 1];
            if (norm) { float nf = normalize(&g_normalizer, I, Q); I *= nf; Q *= nf; }
            float s_i = sync_channel_process(&si, I), s_q = sync_channel_process(&sq, Q);
            float d_i = data_channel_process(&di, I), d_q = data_channel_process(&dq, Q);
            tick_detector_process_sample(g_tick, s_i, s_q);
            marker_detector_process_sample(g_mark, s_i, s_q);
            bcd_time_detector_process_sample(bt, d_i, d_q);
            bcd_freq_detector_process_sample(bf, d_i, d_q);
            if (n % 2500 == 0) sync_detector_periodic_check(g_sync, (float)(n / FS * 1000.0));
        }
    }
    fclose(fp);
    frame_time_t ft = sync_detector_get_frame_time(g_sync);
    printf("END samples=%ld sync=%s conf=%.2f sec=%d ticks=%d markers=%d bcdsyms=%d\n", n, sync_state_name(sync_detector_get_state(g_sync)),
           sync_detector_get_confidence(g_sync), ft.current_second, tick_detector_get_tick_count(g_tick),
           marker_detector_get_marker_count(g_mark), bcd_correlator_get_symbol_count(g_bcd));
    PRINT_TELEM = 1;
    bcd_correlator_destroy(g_bcd);
    return 0;
}
