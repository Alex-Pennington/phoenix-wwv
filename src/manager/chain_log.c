/**
 * @file chain_log.c
 * @brief Air harness text log of the detector chain (see wwv_chain_log.h)
 *
 * WEV and WCLOSE read the BCD correlator's window accumulators: WEV after the
 * correlator has taken the event, WCLOSE inside the symbol callback, before the
 * accumulators are reset. tests/air/bcd_window_trace.py scores them.
 */

#include "wwv_chain_log.h"
#include "bcd_correlator_internal.h"
#include <stdlib.h>
#include <string.h>

struct wwv_chain_log {
    FILE *out;
    wwv_detector_manager_t *mgr;
};

wwv_chain_log_t *wwv_chain_log_create(FILE *out, wwv_detector_manager_t *mgr) {
    wwv_chain_log_t *log = calloc(1, sizeof(*log));
    if (!log) return NULL;
    log->out = out;
    log->mgr = mgr;
    return log;
}

void wwv_chain_log_destroy(wwv_chain_log_t *log) { free(log); }

static void on_tick(const tick_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "TICK n=%d t=%.1f dur=%.1f int=%.1f\n", e->tick_number, e->timestamp_ms, e->duration_ms, e->interval_ms);
}

static void on_tick_marker(const tick_marker_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "TMARK n=%d trail=%.1f lead=%.1f dur=%.1f\n", e->marker_number, e->timestamp_ms, e->start_timestamp_ms, e->duration_ms);
}

static void on_marker(const marker_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "MARK n=%d t=%.1f dur=%.1f lead=%.1f\n", e->marker_number, e->timestamp_ms, e->duration_ms, e->start_timestamp_ms);
}

static void on_tick_marker_candidate(const tick_marker_candidate_t *c, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "CANDT start=%.1f end=%.1f lead_s=%.1f lead_e=%.1f\n",
            c->start_ms, c->end_ms, c->lead_from_start_ms, c->lead_from_end_ms);
}

static void on_marker_candidate(const marker_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "CANDM t=%.1f dur=%.1f lead=%.1f\n", e->timestamp_ms, e->duration_ms, e->start_timestamp_ms);
}

/* Which window a BCD event landed in */
static void trace_event(wwv_chain_log_t *log, char src, float t, float dur) {
    bcd_correlator_t *c = wwv_detector_manager_get_bcd_correlator(log->mgr);
    if (c && c->window_open)
        fprintf(log->out, "WEV src=%c t=%.1f dur=%.1f win_sec=%d win_start=%.1f anchor=%.1f\n",
                src, t, dur, c->current_second, c->window_start_ms, c->window_anchor_ms);
    else fprintf(log->out, "WEV src=%c t=%.1f dur=%.1f win_sec=-1\n", src, t, dur);
}

static void on_bt(const bcd_time_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "BTIME t=%.1f dur=%.1f at=%.1f\n", e->timestamp_ms, e->duration_ms, wwv_detector_manager_get_now_ms(log->mgr));
}

static void on_bt_routed(const bcd_time_event_t *e, void *u) {
    trace_event(u, 'T', e->timestamp_ms, e->duration_ms);
}

static void on_bf(const bcd_freq_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "BFREQ t=%.1f dur=%.1f at=%.1f\n", e->timestamp_ms, e->duration_ms, wwv_detector_manager_get_now_ms(log->mgr));
}

static void on_bf_routed(const bcd_freq_event_t *e, void *u) {
    trace_event(u, 'F', e->timestamp_ms, e->duration_ms);
}

static void on_sym(const bcd_symbol_event_t *e, void *u) {
    wwv_chain_log_t *log = u;
    bcd_correlator_t *c = wwv_detector_manager_get_bcd_correlator(log->mgr);
    fprintf(log->out, "WCLOSE sec=%d win_start=%.1f anchor=%.1f tn=%d tfirst=%.1f tlast=%.1f tdur=%.1f fn=%d ffirst=%.1f flast=%.1f fdur=%.1f est=%.1f\n",
            c->current_second, c->window_start_ms, c->window_anchor_ms,
            c->time_event_count, c->time_first_ms, c->time_last_ms, c->time_duration_sum,
            c->freq_event_count, c->freq_first_ms, c->freq_last_ms, c->freq_duration_sum,
            e->duration_ms);
    fprintf(log->out, "SYMCB sym=%c t=%.1f dur=%.1f conf=%.2f src=%s sec=%d\n", bcd_corr_symbol_char(e->symbol), e->timestamp_ms, e->duration_ms, e->confidence, e->source, e->second);
    fprintf(log->out, "SYMMIN sec=%d minute_start=%.1f pulse_offset=%.1f\n", e->second, e->minute_start_ms, e->pulse_offset_ms);
}

static void on_time(const wwv_td_result_t *r, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "TDRES start=%.1f at=%.1f\n", r->frame_start_ms, wwv_detector_manager_get_now_ms(log->mgr));
}

static void on_state(sync_state_t o, sync_state_t n, float c, void *u) {
    wwv_chain_log_t *log = u;
    fprintf(log->out, "SYNC %s->%s conf=%.2f\n", sync_state_name(o), sync_state_name(n), c);
}

wwv_detector_observer_t wwv_chain_log_observer(wwv_chain_log_t *log) {
    wwv_detector_observer_t obs = {
        .on_tick = on_tick, .on_tick_marker = on_tick_marker, .on_marker = on_marker,
        .on_tick_marker_candidate = on_tick_marker_candidate, .on_marker_candidate = on_marker_candidate,
        .on_bcd_time = on_bt, .on_bcd_time_routed = on_bt_routed,
        .on_bcd_freq = on_bf, .on_bcd_freq_routed = on_bf_routed,
        .on_symbol = on_sym, .on_time = on_time, .on_sync_state = on_state,
        .user = log };
    return obs;
}

void wwv_chain_log_telem(wwv_chain_log_t *log, telem_channel_t channel, const char *csv_line) {
    if (channel == TELEM_BCDS) fprintf(log->out, "TELEM BCDS %s\n", csv_line);
    /* Marker detector purity rejection: PURITY,end_ms,start_ms,window_run_ms,flank,peak */
    else if (channel == TELEM_MARKERS && strncmp(csv_line, "PURITY,", 7) == 0)
        fprintf(log->out, "TELEM MARK %s\n", csv_line);
}

void wwv_chain_log_end(wwv_chain_log_t *log, long samples) {
    wwv_detector_manager_t *mgr = log->mgr;
    sync_detector_t *sync = wwv_detector_manager_get_sync_detector(mgr);
    bcd_correlator_t *bcd = wwv_detector_manager_get_bcd_correlator(mgr);
    frame_time_t ft = sync_detector_get_frame_time(sync);
    fprintf(log->out, "END samples=%ld sync=%s conf=%.2f sec=%d ticks=%d markers=%d bcdsyms=%d\n", samples,
            sync_state_name(sync_detector_get_state(sync)), sync_detector_get_confidence(sync), ft.current_second,
            wwv_detector_manager_get_tick_count(mgr), wwv_detector_manager_get_marker_count(mgr),
            bcd_correlator_get_symbol_count(bcd));
    fprintf(log->out, "BCDWIN silent=%d late_events=%d broken=%d\n", bcd->silent_windows, bcd->late_events,
            bcd->broken_windows);
}
