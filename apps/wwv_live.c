/**
 * @file wwv_live.c
 * @brief Live WWV host: I/Q stream -> detector chain -> sync -> BCD -> UTC, with UDP telemetry
 *
 * Reads complex I/Q from stdin (int16 or float32, interleaved, at --rate) or from an
 * sdr_server over TCP (the PHXI/IQDQ protocol waterfall.c spoke), shifts the
 * carrier to DC, decimates to 50 kHz (detector) and 12 kHz (tones) with
 * iq_frontend, and runs the library's detector manager. Telemetry goes out as UDP
 * broadcast through telem_init() (docs/UDP_TELEMETRY_OUTPUT_PROTOCOL.md).
 *
 * Time: detector timestamps are ms since detector sample 0. Input sample 0 is
 * anchored to the system clock when the first block arrives (arrival time minus
 * the block's duration), or to --t0 for a recording. UTC of a detector time t is
 * anchor + t - front-end group delay. The anchor is logged at start, sent as a
 * CONS telemetry line, and written by --timebase in the form the scoring scripts
 * read (out_utc0_unix = UTC of detector time 0). With --anchor-in, the anchor is
 * the one rsp2_stream.py measured before its hold line (see there), read when the
 * 10 s refinement ends.
 *
 * Clock: once the decoder has CONFIRMED the time of day, every CONFIRMED frame
 * gives a CLOCK line (stdout, --log, and CONS,CLOCK on UDP): UTC of that minute's
 * start less the host's reckoning of it from the anchor, with its sigma
 * (wwv_clock_report.h, docs/WWV_LIVE.md).
 *
 * Gaps: rsp2_stream.py fills samples lost upstream with zeros, so the sample count
 * stays on true time. A run of at least GAP_MIN_S of exact zero samples (s16 or
 * f32) is such a fill: it is passed to wwv_detector_manager_mark_gap() before it is
 * processed, so no event the zeros made or cut reaches sync, the correlator or the
 * decoder, and the log gets "GAP start= len=" before the span's events and
 * "GAPEND ..." with what was held back once the span has settled.
 *
 * See docs/WWV_LIVE.md.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "wwv_detector_manager.h"
#include "wwv_chain_log.h"
#include "wwv_detector_manager_internal.h"   /* the BTIME detector, for --frames */
#include "bcd_internal.h"
#include "iq_frontend.h"
#include "telemetry.h"
#include "wwv_clock_report.h"
#include "version.h"

/*============================================================================
 * sdr_server TCP protocol (waterfall.c 1ec38d1)
 *============================================================================*/

#define MAGIC_PHXI  0x50485849u
#define MAGIC_IQDQ  0x49514451u
#define MAGIC_META  0x4D455441u
#define IQ_FORMAT_S16   1
#define IQ_FORMAT_F32   2
#define IQ_FORMAT_U8    3

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, version, sample_rate, sample_format;
    uint32_t center_freq_lo, center_freq_hi;
    uint32_t reserved[2];
} iq_stream_header_t;

typedef struct {
    uint32_t magic, sequence, num_samples, flags;
} iq_data_frame_t;

typedef struct {
    uint32_t magic, sample_rate, sample_format;
    uint32_t center_freq_lo, center_freq_hi;
    uint32_t gain_reduction, lna_state, reserved;
} iq_metadata_update_t;
#pragma pack(pop)

/*============================================================================
 * Options and state
 *============================================================================*/

typedef struct {
    const char *input;          /* "-" or tcp:HOST:PORT */
    int format;                 /* IQ_FORMAT_S16 / F32 */
    double rate;
    double shift_hz;
    float scale;
    bool agc;
    int udp_port;               /* 0: no UDP */
    bool have_t0;
    double t0;
    const char *log_path;
    const char *timebase_path;
    const char *anchor_in_path;  /* rsp2_stream.py --anchor-out */
    const char *csv_dir;
    const char *frames_path;
    double status_s;
    bool tones;
    double band_khz;            /* --band-khz, for the CLOCK line; 0 when not given */
} options_t;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    options_t opt;
    wwv_detector_manager_t *mgr;
    iq_frontend_t *fe;
    wwv_chain_log_t *chain_log;
    FILE *log;
    wwv_detector_observer_t log_obs;
    FILE *frames;               /* --frames: BTIME detector state per frame */
    float frame_pulse;          /* width of a BTIME event reported on this frame */

    bool anchored;
    bool refining;              /* anchor still taking the lower envelope */
    double anchor_unix;         /* UTC of input sample 0 */
    double lag_min_ms, lag_max_ms;  /* arrival lag behind the anchor, this status interval */
    double lag_said_ms;             /* minimum lag at the last LAG line */
    double utc0;                /* UTC of detector time 0 */
    double hold_s;              /* the producer's hold line (--anchor-in), taken off arrival times */
    uint64_t in_samples;

    /* zero runs (filled gaps) */
    uint64_t zero_run;          /* zero samples held back: a run that reached the end of a block */
    bool in_gap;                /* pushing a fill: its arrival time says nothing of the stream's lag */
    struct { double start_ms, len_ms, close_ms; } open_gaps[WWV_GAP_SPANS];
    int n_open_gaps;
    wwv_gap_drops_t drops_said; /* drop counts at the last GAPEND */
    int n_gaps; double gap_ms_total;
    double next_status_ms;

    /* status */
    sync_state_t sync_state;
    float sync_conf;
    char syms[64];              /* last symbols, newest last */
    int nsyms_total;
    bool have_td;
    wwv_td_result_t td;
    double td_at_ms;

    wwv_clock_anchor_t clock_anchor;   /* the anchor as the CLOCK line takes it */
    wwv_clock_report_t clock;
} app_t;

static app_t A;

static double now_unix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void fmt_utc(double t, char *buf, size_t n, int decimals) {
    time_t s = (time_t)floor(t);
    double frac = t - (double)s;
    struct tm tm;
    gmtime_r(&s, &tm);
    size_t k = strftime(buf, n, "%Y-%m-%dT%H:%M:%S", &tm);
    if (decimals > 0 && k < n) {
        char f[16];
        snprintf(f, sizeof f, "%.*f", decimals, frac);
        if (f[0] == '1') snprintf(f, sizeof f, "0.%0*d", decimals, 0);  /* rounding to 1.0 */
        snprintf(buf + k, n - k, "%sZ", f + 1);
    } else if (k < n) {
        snprintf(buf + k, n - k, "Z");
    }
}

static char state_char(wwv_td_state_t s) {
    switch (s) {
        case WWV_TD_DECODED: return 'D';
        case WWV_TD_CONFIRMED: return 'C';
        case WWV_TD_PREDICTED: return 'P';
        default: return '-';
    }
}

/*============================================================================
 * Observer: status, console lines, chain log
 *============================================================================*/

static void obs_tick(const tick_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_tick(e, A.log_obs.user);
}
static void obs_tick_marker(const tick_marker_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_tick_marker(e, A.log_obs.user);
}
static void obs_marker(const marker_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_marker(e, A.log_obs.user);
    char t[40];
    fmt_utc(A.utc0 + e->start_timestamp_ms / 1000.0, t, sizeof t, 3);
    printf("MARKER   lead %s (stream %.1f ms) dur %.0f ms\n", t, e->start_timestamp_ms, e->duration_ms);
}
static void obs_tick_marker_candidate(const tick_marker_candidate_t *c, void *u) {
    (void)u; if (A.log) A.log_obs.on_tick_marker_candidate(c, A.log_obs.user);
}
static void obs_marker_candidate(const marker_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_marker_candidate(e, A.log_obs.user);
}
static void obs_bt(const bcd_time_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_bcd_time(e, A.log_obs.user);
    A.frame_pulse = e->duration_ms;
}
static void obs_bt_routed(const bcd_time_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_bcd_time_routed(e, A.log_obs.user);
}
static void obs_bf(const bcd_freq_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_bcd_freq(e, A.log_obs.user);
}
static void obs_bf_routed(const bcd_freq_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_bcd_freq_routed(e, A.log_obs.user);
}
static void obs_symbol(const bcd_symbol_event_t *e, void *u) {
    (void)u; if (A.log) A.log_obs.on_symbol(e, A.log_obs.user);
    char c = (e->symbol == BCD_CORR_SYM_NONE) ? '.' : bcd_corr_symbol_char(e->symbol);
    /* a 0/1 from a mid-second detection only: the decoder does not use it */
    if ((e->symbol == BCD_CORR_SYM_ZERO || e->symbol == BCD_CORR_SYM_ONE) &&
        !(e->pulse_offset_ms <= WWV_TD_DEFAULT_BOUNDARY_MS)) c = '~';
    size_t n = strlen(A.syms);
    if (n >= 60) { memmove(A.syms, A.syms + 1, n); n--; }
    A.syms[n] = c; A.syms[n + 1] = '\0';
    A.nsyms_total++;
}
static void obs_time(const wwv_td_result_t *r, void *u) {
    (void)u; if (A.log) A.log_obs.on_time(r, A.log_obs.user);
    A.td = *r; A.have_td = true;
    A.td_at_ms = wwv_detector_manager_get_now_ms(A.mgr);
    char t[40];
    fmt_utc(A.utc0 + r->frame_start_ms / 1000.0, t, sizeof t, 3);
    printf("MINUTE   start %s frame %s\n", t, r->frame);
    char tod[8] = "--:--", date[16] = "----------", dut1[8] = "--", dst[4] = "--", lsw[4] = "-";
    if (r->tod_state != WWV_TD_NONE) snprintf(tod, sizeof tod, "%02d:%02d", r->hour % 100, r->minute % 100);
    if (r->date_state != WWV_TD_NONE)
        snprintf(date, sizeof date, "%04d-%02d-%02d", r->year % 10000, r->month % 100, r->mday % 100);
    if (r->dut1_state != WWV_TD_NONE) snprintf(dut1, sizeof dut1, "%+.1f", (r->dut1_tenths % 100) / 10.0);
    if (r->dst_state != WWV_TD_NONE) snprintf(dst, sizeof dst, "%d%d", r->dst55 & 1, r->dst2 & 1);
    if (r->lsw_state != WWV_TD_NONE) snprintf(lsw, sizeof lsw, "%d", r->lsw & 1);
    printf("         tod %s %s (%.1f)  date %s %s (%.1f)  DUT1 %s %s  DST %s %s  LSW %s %s\n",
           tod, wwv_td_state_name(r->tod_state), r->tod_margin,
           date, wwv_td_state_name(r->date_state), r->date_margin,
           dut1, wwv_td_state_name(r->dut1_state), dst, wwv_td_state_name(r->dst_state),
           lsw, wwv_td_state_name(r->lsw_state));
    if (r->tod_state != WWV_TD_NONE) {
        /* system clock at the minute's start against the decoded minute, mod one day */
        double sys = A.utc0 + r->frame_start_ms / 1000.0;
        double tod = r->hour * 3600.0 + r->minute * 60.0;
        double d = fmod(fmod(sys, 86400.0) - tod + 86400.0 * 1.5, 86400.0) - 43200.0;
        printf("         system clock at the decoded minute's start: %+.3f s\n", d);
    }
    char line[512];
    if (wwv_clock_report(&A.clock, &A.clock_anchor, wwv_detector_manager_get_sync_detector(A.mgr), r,
                         now_unix(), line, sizeof line) > 0) {
        fputs(line, stdout);
        if (A.log) { fputs(line, A.log); fflush(A.log); }
        telem_sendf(TELEM_CONSOLE, "CLOCK,%s", line + strlen("CLOCK "));
        fflush(stdout);
    }
}
static void obs_sync(sync_state_t o, sync_state_t n, float c, void *u) {
    (void)u; if (A.log) A.log_obs.on_sync_state(o, n, c, A.log_obs.user);
    A.sync_state = n; A.sync_conf = c;
    char t[40];
    fmt_utc(A.utc0 + wwv_detector_manager_get_now_ms(A.mgr) / 1000.0, t, sizeof t, 1);
    printf("SYNC     %s %s -> %s conf %.2f\n", t, sync_state_name(o), sync_state_name(n), c);
}

static void telem_tap(telem_channel_t ch, const char *line, void *u) {
    (void)u;
    if (A.chain_log) wwv_chain_log_telem(A.chain_log, ch, line);
}

/*============================================================================
 * Status line
 *============================================================================*/

#define LAG_STEP_MS 250.0   /* a LAG line for each this much growth of the minimum lag */

static void print_status(double stream_ms) {
    char t[40];
    fmt_utc(A.utc0 + stream_ms / 1000.0, t, sizeof t, 0);
    sync_detector_t *sd = wwv_detector_manager_get_sync_detector(A.mgr);
    frame_time_t ft = sync_detector_get_frame_time(sd);
    printf("STATUS   %s t=%.0fs sync=%s(%.2f) sec=%d ticks=%d marks=%d syms=%d last=%s",
           t + 11, stream_ms / 1000.0, sync_state_name(sync_detector_get_state(sd)),
           sync_detector_get_confidence(sd), ft.current_second,
           wwv_detector_manager_get_tick_count(A.mgr), wwv_detector_manager_get_marker_count(A.mgr),
           A.nsyms_total, A.syms[0] ? A.syms + (strlen(A.syms) > 20 ? strlen(A.syms) - 20 : 0) : "-");
    if (A.have_td && A.td.tod_state != WWV_TD_NONE) {
        /* the decoded minute carried forward to now */
        double el = (stream_ms - A.td.frame_start_ms) / 1000.0;
        int tod = (int)floor(A.td.hour * 3600.0 + A.td.minute * 60.0 + el);
        tod = ((tod % 86400) + 86400) % 86400;
        printf(" | UTC %02d:%02d:%02d tod=%s", tod / 3600, tod / 60 % 60, tod % 60,
               wwv_td_state_name(A.td.tod_state));
        if (A.td.date_state != WWV_TD_NONE)
            printf(" date=%04d-%02d-%02d %s", A.td.year, A.td.month, A.td.mday, wwv_td_state_name(A.td.date_state));
        else
            printf(" date=NONE");
        printf(" [%c%c%c%c%c]", state_char(A.td.tod_state), state_char(A.td.date_state),
               state_char(A.td.dut1_state), state_char(A.td.dst_state), state_char(A.td.lsw_state));
    } else {
        printf(" | UTC not decoded");
    }
    bool lag_grew = false;
    double lag_min = A.lag_min_ms;
    if (!A.opt.have_t0 && A.lag_max_ms >= A.lag_min_ms) {
        /* blocks arrive this long after their last sample's time on the anchored
         * counter; a minimum that grows means samples lost upstream or clock drift */
        printf(" | lag %+.0f..%+.0f ms", A.lag_min_ms, A.lag_max_ms);
        lag_grew = !A.refining && A.lag_min_ms >= A.lag_said_ms + LAG_STEP_MS;
    }
    A.lag_min_ms = 1e30; A.lag_max_ms = -1e30;
    printf("\n");
    if (lag_grew) {
        /* Said once per LAG_STEP_MS of growth. The least-delayed block of the
         * interval arrives this late, so samples are missing from the counter (lost
         * before wwv_live) or the input is backed up; either way UTC read off the
         * sample counter is late by about this much. rsp2_stream's "behind wall
         * clock" line says which. */
        printf("LAG      %s stream time is %+.0f ms behind the system clock (%+.0f since the last LAG line): "
               "UTC from the sample counter is late by that\n", t + 11, lag_min, lag_min - A.lag_said_ms);
        A.lag_said_ms = lag_min;
    }
    fflush(stdout);
}

/*============================================================================
 * Sample path
 *============================================================================*/

/* One --frames record per BTIME frame (5.12 ms), 9 float32, native byte order:
 *   t_ms (frame start, stream time), energy (bucket value), noise_floor,
 *   threshold_high, threshold_low, state (0 idle, 1 in pulse, 2 cooldown),
 *   start_threshold (what an idle frame must exceed: the highest of
 *   threshold_high, BCD_TIME_EDGE_FRACTION x pulse_level and
 *   BCD_TIME_START_LOW_RATIO x the low level), pulse_level, pulse
 *   (duration_ms of a BTIME event reported on this frame, else 0: the frame where
 *   an edge-width event ends, or, for an event held for the step fit, the frame
 *   176 frames after the frame that crossed its start threshold, with the fitted width;
 *   BCD_TIME_FIT_* in include/bcd_time_detector.h).
 * The first six fields are tests/air/btime_frames's; its seventh (ref) needs
 * the data-channel samples and is not written here. */
static void write_frame(const bcd_time_detector_t *bt) {
    float start = bt->start_threshold;
    float rec[9] = {
        (float)((double)(bt->frame_count - 1) * bcd_time_detector_get_frame_duration_ms()),
        bt->current_energy, bt->noise_floor, bt->threshold_high, bt->threshold_low,
        (float)bt->state, start, bt->pulse_level, A.frame_pulse };
    fwrite(rec, sizeof rec, 1, A.frames);
    A.frame_pulse = 0.0f;
}

static void det_sample(float i, float q, void *u) {
    (void)u;
    wwv_detector_manager_process_detector_sample(A.mgr, i, q);
    if (A.frames && A.mgr->bcd_time && A.mgr->bcd_time->buffer_idx == 0 &&
        A.mgr->bcd_time->frame_count > 0)
        write_frame(A.mgr->bcd_time);     /* a frame just ran */
    double ms = wwv_detector_manager_get_now_ms(A.mgr);
    if (ms >= A.next_status_ms) {
        print_status(ms);
        A.next_status_ms += A.opt.status_s * 1000.0;
    }
}

static void disp_sample(float i, float q, void *u) {
    (void)u;
    wwv_detector_manager_process_display_sample(A.mgr, i, q);
}

#define ANCHOR_REFINE_S 10.0

/* Set the anchor (UTC of input sample 0), log it, send it, write --timebase.
 * src names it for the CLOCK line (rsp2_stream, wwv_live, t0); NULL while the
 * anchor is still refining, which the CLOCK line never uses. */
static void set_anchor(double sample0_unix, const char *how, const char *src) {
    double gd = iq_frontend_group_delay_s(A.fe);
    A.anchor_unix = sample0_unix;
    A.utc0 = A.anchor_unix - gd;
    A.anchored = true;
    A.clock_anchor = (wwv_clock_anchor_t){ .set = src != NULL, .in_sample0_unix = sample0_unix,
                                           .group_delay_ms = gd * 1000.0, .band_khz = A.opt.band_khz };
    if (src) {
        snprintf(A.clock_anchor.source, sizeof A.clock_anchor.source, "%s", src);
        /* for tests/air/replay_events, which then prints the CLOCK lines too */
        if (A.log)
            fprintf(A.log, "ANCHOR in_sample0_unix=%.6f group_delay_ms=%.6f band_khz=%g source=%s\n",
                    sample0_unix, gd * 1000.0, A.opt.band_khz, src);
    }
    char a[40], c[40];
    fmt_utc(A.anchor_unix, a, sizeof a, 6);
    fmt_utc(A.utc0, c, sizeof c, 6);
    printf("ANCHOR   input sample 0 = %s: %s\n", a, how);
    printf("ANCHOR   detector time 0 = %s (front-end group delay %.3f ms)\n", c, gd * 1000.0);
    printf("ANCHOR   UTC(t) = %.6f + t_ms/1000; sample counter at %.0f Hz, no clock-rate correction\n",
           A.utc0, A.opt.rate);
    telem_sendf(TELEM_CONSOLE, "ANCHOR,%.6f,%.6f,%.3f,%.0f\n", A.anchor_unix, A.utc0, gd * 1000.0, A.opt.rate);
    if (A.opt.timebase_path) {
        FILE *f = fopen(A.opt.timebase_path, "w");
        if (f) {
            fprintf(f, "{\n \"source\": \"wwv_live\",\n \"anchor\": \"%s\",\n \"in_sample0_unix\": %.6f,\n"
                       " \"frontend_group_delay_ms\": %.6f,\n \"out_utc0_unix\": %.6f,\n \"out_utc0_iso\": \"%s\"\n}\n",
                    how, A.anchor_unix, gd * 1000.0, A.utc0, c);
            fclose(f);
        } else {
            fprintf(stderr, "wwv_live: %s: %s\n", A.opt.timebase_path, strerror(errno));
        }
    }
    fflush(stdout);
}

/* A block of n samples arrived at `arrival` (system clock). Its last sample was
 * taken no later than that, so arrival - (samples so far)/rate is the time of
 * sample 0 plus a latency >= 0. The first block anchors; for ANCHOR_REFINE_S of
 * stream the anchor follows the lowest such value (least-delayed block), then it
 * is fixed and logged again. */
/* --anchor-in: in_sample0_unix and hold_s from rsp2_stream's JSON; false if absent */
static bool read_anchor_in(double *sample0, double *hold) {
    FILE *f = fopen(A.opt.anchor_in_path, "r");
    if (!f) return false;
    char buf[4096];
    size_t k = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[k] = '\0';
    const char *a = strstr(buf, "\"in_sample0_unix\":"), *h = strstr(buf, "\"hold_s\":");
    if (!a || !h) return false;
    *sample0 = strtod(a + strlen("\"in_sample0_unix\":"), NULL);
    *hold = strtod(h + strlen("\"hold_s\":"), NULL);
    return *sample0 > 0.0 && *hold >= 0.0;
}

static void time_block(size_t n, double arrival) {
    double d = arrival - (double)(A.in_samples + n) / A.opt.rate;
    if (!A.anchored) {
        if (A.opt.have_t0) {
            char how[64];
            snprintf(how, sizeof how, "--t0 %.6f", A.opt.t0);
            set_anchor(A.opt.t0, how, "t0");
            return;
        }
        set_anchor(d, "system clock at the first block minus its duration (refining)", NULL);
        A.refining = true;
        A.lag_min_ms = 1e30; A.lag_max_ms = -1e30;
    }
    if (A.opt.have_t0) return;
    if (A.refining) {
        if (d < A.anchor_unix) { A.anchor_unix = d; A.utc0 = d - iq_frontend_group_delay_s(A.fe); }
        if ((double)A.in_samples / A.opt.rate >= ANCHOR_REFINE_S) {
            A.refining = false;
            char how[160];
            double s0, hold;
            if (A.opt.anchor_in_path && read_anchor_in(&s0, &hold)) {
                /* rsp2_stream measured it before its hold line; its own arrivals
                 * here are late by the hold, which the lag now leaves out */
                snprintf(how, sizeof how, "rsp2_stream: least-delayed block over the first %.0f s, "
                         "before its %.3f s hold line", ANCHOR_REFINE_S, hold);
                A.hold_s = hold;
                set_anchor(s0, how, "rsp2_stream");
            } else {
                if (A.opt.anchor_in_path)
                    fprintf(stderr, "wwv_live: --anchor-in %s: no anchor read; using this end's, which is "
                            "late by the producer's hold line\n", A.opt.anchor_in_path);
                snprintf(how, sizeof how, "least-delayed block over the first %.0f s (arrival minus stream time)",
                         ANCHOR_REFINE_S);
                set_anchor(A.anchor_unix, how, "wwv_live");
            }
        }
    }
    if (A.in_gap) return;
    double lag = (d - A.hold_s - A.anchor_unix) * 1000.0;   /* hold_s: read with the anchor */
    if (lag < A.lag_min_ms) A.lag_min_ms = lag;
    if (lag > A.lag_max_ms) A.lag_max_ms = lag;
}

/* Convert n samples of the given format into floats and push them */
static void push_samples(const void *buf, size_t n, int format, double arrival) {
    static float *f = NULL;
    static size_t cap = 0;
    if (n > cap) { free(f); cap = n; f = malloc(sizeof(float) * 2 * cap); }
    if (format == IQ_FORMAT_S16) {
        const int16_t *s = buf;
        for (size_t k = 0; k < 2 * n; k++) f[k] = (float)s[k] / 32768.0f;
    } else if (format == IQ_FORMAT_U8) {
        const uint8_t *s = buf;
        for (size_t k = 0; k < 2 * n; k++) f[k] = ((float)s[k] - 128.0f) / 128.0f;
    } else {
        memcpy(f, buf, sizeof(float) * 2 * n);
    }
    time_block(n, arrival);
    A.in_samples += n;
    iq_frontend_push(A.fe, f, n, det_sample, A.opt.tones ? disp_sample : NULL, NULL);
}

/*----------------------------------------------------------------------------
 * Filled gaps: runs of exact zeros
 *----------------------------------------------------------------------------*/

#define GAP_MIN_S     0.0005   /* a zero run at least this long is a filled gap */
#define GAP_MIN_SAMPLES 64
#define GAPEND_WAIT_MS 5000.0  /* after a span's settle time, for the slowest event report */

static size_t gap_min_samples(void) {
    size_t m = (size_t)(GAP_MIN_S * A.opt.rate);
    return m < GAP_MIN_SAMPLES ? GAP_MIN_SAMPLES : m;
}

static bool is_zero(const void *buf, size_t k, int format) {
    if (format == IQ_FORMAT_S16) { const int16_t *s = buf; return s[2 * k] == 0 && s[2 * k + 1] == 0; }
    if (format == IQ_FORMAT_F32) { const float *s = buf; return s[2 * k] == 0.0f && s[2 * k + 1] == 0.0f; }
    return false;
}

/* GAPEND lines for spans whose settle time has passed (all of them when `all`) */
static void close_gaps(bool all) {
    double now = wwv_detector_manager_get_now_ms(A.mgr);
    int k = 0;
    while (k < A.n_open_gaps) {
        if (!all && now < A.open_gaps[k].close_ms) { k++; continue; }
        /* events held back since the last GAPEND: spans that overlap in time share them */
        wwv_gap_drops_t d = wwv_detector_manager_get_gap_drops(A.mgr), p = A.drops_said;
        wwv_gap_drops_t x = { d.tick - p.tick, d.tmark - p.tmark, d.marker - p.marker,
                              d.candidate - p.candidate, d.btime - p.btime, d.bfreq - p.bfreq };
        A.drops_said = d;
        if (A.log)
            fprintf(A.log, "GAPEND start=%.1f len=%.1f tick=%d tmark=%d mark=%d cand=%d btime=%d bfreq=%d\n",
                    A.open_gaps[k].start_ms, A.open_gaps[k].len_ms, x.tick, x.tmark, x.marker, x.candidate,
                    x.btime, x.bfreq);
        printf("GAPEND   stream %.1f s, %.0f ms: held back tick %d, tick-marker %d, marker %d, candidate %d, "
               "BTIME %d, BFREQ %d\n", A.open_gaps[k].start_ms / 1000.0, A.open_gaps[k].len_ms,
               x.tick, x.tmark, x.marker, x.candidate, x.btime, x.bfreq);
        memmove(&A.open_gaps[k], &A.open_gaps[k + 1], sizeof A.open_gaps[0] * (size_t)(A.n_open_gaps - k - 1));
        A.n_open_gaps--;
    }
}

static int bytes_per_sample(int format);

/* Push n zero samples as they are (a run too short to be a fill) */
static void push_zeros(uint64_t n, double arrival) {
    static const float zf[2 * 4096];
    while (n > 0) {
        size_t m = n > 4096 ? 4096 : (size_t)n;
        push_samples(zf, m, IQ_FORMAT_F32, arrival);
        n -= m;
    }
}

/* Push n zero samples that are a filled gap: mark it first, then process it */
static void push_gap(uint64_t n, int format, double arrival) {
    double gd_ms = iq_frontend_group_delay_s(A.fe) * 1000.0;
    double start_ms = (double)A.in_samples / A.opt.rate * 1000.0 + gd_ms;
    double len_ms = (double)n / A.opt.rate * 1000.0;
    wwv_detector_manager_mark_gap(A.mgr, start_ms, len_ms);
    if (A.log) fprintf(A.log, "GAP start=%.1f len=%.1f\n", start_ms, len_ms);
    char t[40];
    fmt_utc(A.utc0 + start_ms / 1000.0, t, sizeof t, 3);
    printf("GAP      %s stream %.1f s: %.1f ms of samples lost upstream (filled); "
           "events there are not evidence\n", t, start_ms / 1000.0, len_ms);
    if (A.n_open_gaps == WWV_GAP_SPANS) close_gaps(true);
    A.open_gaps[A.n_open_gaps].start_ms = start_ms;
    A.open_gaps[A.n_open_gaps].len_ms = len_ms;
    A.open_gaps[A.n_open_gaps].close_ms = start_ms + len_ms + WWV_GAP_SETTLE_MS + GAPEND_WAIT_MS;
    A.n_open_gaps++;
    A.n_gaps++; A.gap_ms_total += len_ms;
    (void)format;
    A.in_gap = true;          /* a held run goes in at once, stamped with a later block's arrival */
    push_zeros(n, arrival);
    A.in_gap = false;
}

/* Push a block, taking its zero runs out as gaps. A run that reaches the block's
 * end is held back until it ends, so it is marked once, whole. */
static void push_raw(const void *buf, size_t n, int format, double arrival) {
    if (A.n_open_gaps) close_gaps(false);
    if (format != IQ_FORMAT_S16 && format != IQ_FORMAT_F32) { push_samples(buf, n, format, arrival); return; }
    size_t bps = (size_t)bytes_per_sample(format);
    const uint8_t *b = buf;
    size_t k = 0, gmin = gap_min_samples();
    while (k < n) {
        size_t j = k;
        while (j < n && !is_zero(buf, j, format)) j++;
        if (j > k) {
            if (A.zero_run) {    /* the held run ended at this block's start */
                if (A.zero_run >= gmin) push_gap(A.zero_run, format, arrival);
                else push_zeros(A.zero_run, arrival);
                A.zero_run = 0;
            }
            push_samples(b + k * bps, j - k, format, arrival);
            k = j;
        }
        size_t z = k;
        while (z < n && is_zero(buf, z, format)) z++;
        if (z == k) continue;
        if (z == n) { A.zero_run += z - k; break; }   /* may continue in the next block */
        uint64_t run = A.zero_run + (z - k);
        A.zero_run = 0;
        if (run >= gmin) push_gap(run, format, arrival);
        else push_zeros(run, arrival);
        k = z;
    }
}

static int bytes_per_sample(int format) {
    return format == IQ_FORMAT_S16 ? 4 : format == IQ_FORMAT_F32 ? 8 : 2;
}

static bool make_frontend(void) {
    iq_frontend_destroy(A.fe);
    iq_frontend_config_t fc = { .in_rate = A.opt.rate, .shift_hz = A.opt.shift_hz,
                                .scale = A.opt.scale, .display = A.opt.tones };
    char err[160];
    A.fe = iq_frontend_create(&fc, err, sizeof err);
    if (!A.fe) { fprintf(stderr, "wwv_live: %s\n", err); return false; }
    printf("FRONTEND %s, shift %.3f Hz, scale %g, group delay %.3f ms\n",
           iq_frontend_describe(A.fe), A.opt.shift_hz, A.opt.scale, iq_frontend_group_delay_s(A.fe) * 1000.0);
    return true;
}

/*============================================================================
 * Inputs
 *============================================================================*/

/* Read exactly n bytes; false on EOF, error or stop */
static bool read_exact(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) { if (g_stop) return false; continue; }
            return false;
        }
        p += r; n -= (size_t)r;
    }
    return true;
}

static void run_stdin(void) {
    int bps = bytes_per_sample(A.opt.format);
    size_t block = 16384;
    uint8_t *buf = malloc(block * bps);
    size_t have = 0;
    while (!g_stop) {
        ssize_t r = read(0, buf + have, block * bps - have);
        double arrival = now_unix();
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("wwv_live: stdin");
            break;
        }
        have += (size_t)r;
        size_t n = have / bps;
        if (n == 0) continue;
        push_raw(buf, n, A.opt.format, arrival);
        size_t used = n * bps;
        memmove(buf, buf + used, have - used);
        have -= used;
    }
    free(buf);
}

static int tcp_connect(const char *spec) {
    char host[256];
    const char *colon = strrchr(spec, ':');
    if (!colon || colon == spec || (size_t)(colon - spec) >= sizeof host) return -1;
    memcpy(host, spec, colon - spec);
    host[colon - spec] = '\0';
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void run_tcp(const char *spec) {
    int fd = tcp_connect(spec);
    if (fd < 0) { fprintf(stderr, "wwv_live: cannot connect to %s\n", spec); return; }
    printf("INPUT    connected to %s\n", spec);
    iq_stream_header_t h;
    if (!read_exact(fd, &h, sizeof h) || h.magic != MAGIC_PHXI) {
        fprintf(stderr, "wwv_live: no PHXI stream header from %s\n", spec);
        close(fd); return;
    }
    uint64_t fc = ((uint64_t)h.center_freq_hi << 32) | h.center_freq_lo;
    printf("INPUT    PHXI v%u rate %u Hz format %u centre %llu Hz\n", h.version, h.sample_rate,
           h.sample_format, (unsigned long long)fc);
    if (h.sample_format < IQ_FORMAT_S16 || h.sample_format > IQ_FORMAT_U8) {
        fprintf(stderr, "wwv_live: unknown sample format %u\n", h.sample_format);
        close(fd); return;
    }
    int format = (int)h.sample_format;
    A.opt.rate = h.sample_rate;
    if (!make_frontend()) { close(fd); return; }
    uint8_t *buf = NULL; size_t cap = 0;
    bool have_seq = false; uint32_t last_seq = 0; uint64_t gaps = 0;
    while (!g_stop) {
        uint32_t magic;
        if (!read_exact(fd, &magic, 4)) break;
        if (magic == MAGIC_META) {
            iq_metadata_update_t m;
            if (!read_exact(fd, ((uint8_t *)&m) + 4, sizeof m - 4)) break;
            uint64_t f = ((uint64_t)m.center_freq_hi << 32) | m.center_freq_lo;
            printf("INPUT    META rate %u format %u centre %llu Hz GR %u LNA %u\n", m.sample_rate,
                   m.sample_format, (unsigned long long)f, m.gain_reduction, m.lna_state);
            wwv_detector_manager_log_metadata(A.mgr, f, m.sample_rate, m.gain_reduction, m.lna_state);
            if (m.sample_rate != (uint32_t)A.opt.rate || (int)m.sample_format != format) {
                /* The detector clock keeps counting; a new rate restarts the filters,
                 * and the anchor no longer maps detector time to UTC exactly. */
                fprintf(stderr, "wwv_live: stream rate/format changed mid-run; timestamps after this are approximate\n");
                format = (int)m.sample_format;
                A.opt.rate = m.sample_rate;
                if (!make_frontend()) break;
            }
            continue;
        }
        if (magic != MAGIC_IQDQ) {
            fprintf(stderr, "wwv_live: unknown frame magic 0x%08X, stopping\n", magic);
            break;
        }
        iq_data_frame_t fr;
        if (!read_exact(fd, ((uint8_t *)&fr) + 4, sizeof fr - 4)) break;
        if (have_seq && fr.sequence != last_seq + 1) {
            gaps++;
            fprintf(stderr, "wwv_live: IQDQ sequence gap %u -> %u (%llu gaps)\n", last_seq, fr.sequence,
                    (unsigned long long)gaps);
        }
        have_seq = true; last_seq = fr.sequence;
        size_t bytes = (size_t)fr.num_samples * bytes_per_sample(format);
        if (bytes > cap) { free(buf); cap = bytes; buf = malloc(cap); }
        if (!read_exact(fd, buf, bytes)) break;
        push_raw(buf, fr.num_samples, format, now_unix());
    }
    free(buf);
    close(fd);
}

/*============================================================================
 * Main
 *============================================================================*/

static void usage(const char *p) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  -i, --input SRC        '-' (stdin, default) or tcp:HOST:PORT (sdr_server PHXI/IQDQ)\n"
        "  -f, --format FMT       stdin sample format: s16 (default; scaled /32768) or f32\n"
        "  -r, --rate HZ          stdin sample rate, a whole multiple of 50000 (default 2000000)\n"
        "  -s, --shift HZ         carrier offset in the input: +HZ is moved to DC (default 0)\n"
        "  -g, --scale X          gain after the /32768 scaling (default 1)\n"
        "  -a, --agc              waterfall.c slow AGC before the channel filters\n"
        "  -p, --udp-port N       telemetry UDP broadcast port (default 3005; 0 = off)\n"
        "  -T, --t0 UNIX          UTC of input sample 0 (recordings; default: system clock)\n"
        "  -l, --log FILE         air-harness event log (score.py and tests/air/*.py read it)\n"
        "  -j, --timebase FILE    JSON with out_utc0_unix for the scoring scripts\n"
        "      --anchor-in FILE   rsp2_stream.py --anchor-out: its anchor (taken before its hold\n"
        "                         line) replaces this end's when the 10 s refinement ends\n"
        "  -c, --csv-dir DIR      detector CSV logs (default none)\n"
        "  -F, --frames FILE      BTIME detector state per 5.12 ms frame, 9 float32 (36 B,\n"
        "                         7 kB/s; layout at write_frame() in apps/wwv_live.c)\n"
        "  -S, --status SECONDS   status line period in stream time (default 10)\n"
        "      --no-tones         skip the 12 kHz tone trackers\n"
        "      --band-khz KHZ     the WWV carrier received, for the CLOCK line's band_khz\n"
        "  -h, --help\n", p);
}

int main(int argc, char **argv) {
    A.opt = (options_t){ .input = "-", .format = IQ_FORMAT_S16, .rate = 2000000.0, .scale = 1.0f,
                         .udp_port = TELEM_DEFAULT_PORT, .status_s = 10.0, .tones = true };
    static const struct option lo[] = {
        {"input", 1, 0, 'i'}, {"format", 1, 0, 'f'}, {"rate", 1, 0, 'r'}, {"shift", 1, 0, 's'},
        {"scale", 1, 0, 'g'}, {"agc", 0, 0, 'a'}, {"udp-port", 1, 0, 'p'}, {"t0", 1, 0, 'T'},
        {"log", 1, 0, 'l'}, {"timebase", 1, 0, 'j'}, {"csv-dir", 1, 0, 'c'}, {"frames", 1, 0, 'F'}, {"status", 1, 0, 'S'},
        {"no-tones", 0, 0, 'N'}, {"anchor-in", 1, 0, 'A'}, {"band-khz", 1, 0, 'B'}, {"help", 0, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "i:f:r:s:g:ap:T:l:j:c:F:S:h", lo, NULL)) != -1) {
        switch (c) {
            case 'i': A.opt.input = optarg; break;
            case 'f':
                if (!strcmp(optarg, "s16")) A.opt.format = IQ_FORMAT_S16;
                else if (!strcmp(optarg, "f32") || !strcmp(optarg, "cf32")) A.opt.format = IQ_FORMAT_F32;
                else { usage(argv[0]); return 2; }
                break;
            case 'r': A.opt.rate = atof(optarg); break;
            case 's': A.opt.shift_hz = atof(optarg); break;
            case 'g': A.opt.scale = (float)atof(optarg); break;
            case 'a': A.opt.agc = true; break;
            case 'p': A.opt.udp_port = atoi(optarg); break;
            case 'T': A.opt.have_t0 = true; A.opt.t0 = atof(optarg); break;
            case 'l': A.opt.log_path = optarg; break;
            case 'j': A.opt.timebase_path = optarg; break;
            case 'c': A.opt.csv_dir = optarg; break;
            case 'F': A.opt.frames_path = optarg; break;
            case 'S': A.opt.status_s = atof(optarg); break;
            case 'N': A.opt.tones = false; break;
            case 'A': A.opt.anchor_in_path = optarg; break;
            case 'B': A.opt.band_khz = atof(optarg); break;
            default: usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (A.opt.status_s <= 0) A.opt.status_s = 10.0;
    bool tcp = strncmp(A.opt.input, "tcp:", 4) == 0;
    if (!tcp && strcmp(A.opt.input, "-") != 0) { usage(argv[0]); return 2; }

    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    printf("wwv_live %s: input %s, %s, AGC %s, UDP %s%d\n", PHOENIX_VERSION_FULL,
           tcp ? A.opt.input : "stdin", tcp ? "format/rate from the server" :
           (A.opt.format == IQ_FORMAT_S16 ? "s16" : "f32"), A.opt.agc ? "on" : "off",
           A.opt.udp_port ? "port " : "off ", A.opt.udp_port);

    if (A.opt.log_path) {
        A.log = fopen(A.opt.log_path, "w");
        if (!A.log) { fprintf(stderr, "wwv_live: %s: %s\n", A.opt.log_path, strerror(errno)); return 2; }
    }
    if (A.opt.frames_path) {
        A.frames = fopen(A.opt.frames_path, "wb");
        if (!A.frames) { fprintf(stderr, "wwv_live: %s: %s\n", A.opt.frames_path, strerror(errno)); return 2; }
    }
    /* Detector creation prints go to the log when there is one, as in the harness */
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = A.opt.csv_dir;
    cfg.enable_tone_trackers = A.opt.tones;
    cfg.enable_correlators = false;
    cfg.enable_slow_marker = false;
    cfg.enable_agc = A.opt.agc;
    cfg.verbose = false;
    int saved_stdout = -1;
    if (A.log) { fflush(stdout); saved_stdout = dup(1); dup2(fileno(A.log), 1); }
    A.mgr = wwv_detector_manager_create(&cfg);
    if (A.log) { fflush(stdout); dup2(saved_stdout, 1); close(saved_stdout); }
    if (!A.mgr) { fprintf(stderr, "wwv_live: detector manager create failed\n"); return 2; }
    if (A.log) {
        A.chain_log = wwv_chain_log_create(A.log, A.mgr);
        A.log_obs = wwv_chain_log_observer(A.chain_log);
        if (A.opt.agc) fprintf(A.log, "CONFIG NORM=1 FILE=wwv_live FS=%.0f\n", 50000.0);
        else fprintf(A.log, "CONFIG FILE=wwv_live FS=%.0f\n", 50000.0);
    }
    const wwv_detector_observer_t obs = {
        .on_tick = obs_tick, .on_tick_marker = obs_tick_marker, .on_marker = obs_marker,
        .on_tick_marker_candidate = obs_tick_marker_candidate, .on_marker_candidate = obs_marker_candidate,
        .on_bcd_time = obs_bt, .on_bcd_time_routed = obs_bt_routed,
        .on_bcd_freq = obs_bf, .on_bcd_freq_routed = obs_bf_routed,
        .on_symbol = obs_symbol, .on_time = obs_time, .on_sync_state = obs_sync };
    wwv_detector_manager_set_observer(A.mgr, &obs);

    telem_set_tap(telem_tap, NULL);
    if (A.opt.udp_port > 0 && !telem_init(A.opt.udp_port))
        fprintf(stderr, "wwv_live: telemetry init failed; continuing without UDP\n");

    A.next_status_ms = A.opt.status_s * 1000.0;
    if (tcp) {
        run_tcp(A.opt.input + 4);
    } else {
        if (!make_frontend()) return 2;
        run_stdin();
    }

    if (A.zero_run) {     /* a gap at the very end */
        if (A.zero_run >= gap_min_samples()) push_gap(A.zero_run, A.opt.format, now_unix());
        A.zero_run = 0;
    }
    close_gaps(true);
    if (A.n_gaps)
        printf("GAPS     %d filled gaps, %.1f ms in all\n", A.n_gaps, A.gap_ms_total);
    uint64_t ns = wwv_detector_manager_get_detector_samples(A.mgr);
    print_status(wwv_detector_manager_get_now_ms(A.mgr));
    printf("END      %s: %llu input samples, %llu detector samples (%.1f s)\n",
           g_stop ? "stopped by signal" : "end of input",
           (unsigned long long)A.in_samples, (unsigned long long)ns, ns / 50000.0);
    if (A.chain_log) wwv_chain_log_end(A.chain_log, (long)ns);
    wwv_detector_manager_destroy(A.mgr);      /* closes the last BCD window */
    telem_set_tap(NULL, NULL);
    telem_cleanup();
    if (A.chain_log) wwv_chain_log_destroy(A.chain_log);
    if (A.log) fclose(A.log);
    if (A.frames) fclose(A.frames);
    iq_frontend_destroy(A.fe);
    return 0;
}
