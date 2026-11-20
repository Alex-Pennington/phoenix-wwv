/* Frame replay: re-runs the BCD time detector's state machine on recorded frames.
 *
 *   btime_replay [-8] frames.f32 out.f32 [in_log out_log]
 *
 * frames.f32 is a per-frame record of the BTIME detector: wwv_live --frames
 * (9 float32 per frame, the default) or tests/air/btime_frames (-8: 8 float32).
 * Field 1 of both is the frame's bucket energy, which the data channel and the FFT
 * fix before the state machine sees it; nothing the state machine does feeds back
 * into it. So feeding the recorded energies, one frame at a time, through the
 * library's bcd_time_run_state_machine() gives what the detector under test would
 * have done on that signal.
 *
 * out.f32 gets one record per frame in the wwv_live --frames layout (9 float32,
 * see write_frame() in apps/wwv_live.c), from the replayed detector.
 *
 * With in_log / out_log (the same run's wwv_live --log), out_log is in_log with its
 * BTIME lines replaced by the replayed detector's events, each placed among the other
 * lines by its report time (at=). Run tests/air/replay_events on out_log to score
 * the chain on the replayed events.
 *
 * stderr: frames, events, and how many frames' state and event widths differ from
 * the record. On the code that wrote the record, both must be 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bcd_time_detector.h"
#include "bcd_internal.h"

#define FR_MS ((double)BCD_TIME_FFT_SIZE * 1000.0 / BCD_TIME_SAMPLE_RATE)

typedef struct { double t, dur, at; } ev_t;
static ev_t *evs; static size_t nev, cap;
static uint64_t cur_frame;
static float g_pulse;

static void on_bt(const bcd_time_event_t *e, void *u) {
    (void)u;
    g_pulse = e->duration_ms;
    if (nev == cap) { cap = cap ? 2 * cap : 4096; evs = realloc(evs, cap * sizeof *evs); }
    /* the manager's now_ms at the sample that completed this frame */
    double at = (double)((cur_frame + 1) * BCD_TIME_FFT_SIZE - 1) * 1000.0 / BCD_TIME_SAMPLE_RATE;
    evs[nev++] = (ev_t){ e->timestamp_ms, e->duration_ms, at };
}

/* The time replay_events delivers a detector event line at (BFREQ at=, TICK and
 * MARK t=, TMARK trail=); 0 for the lines it does not read. */
static int line_time(const char *l, double *out) {
    const char *key = NULL;
    if (strncmp(l, "BFREQ ", 6) == 0) key = " at=";
    else if (strncmp(l, "TICK ", 5) == 0 || strncmp(l, "MARK ", 5) == 0) key = " t=";
    else if (strncmp(l, "TMARK ", 6) == 0) key = " trail=";
    const char *p = key ? strstr(l, key) : NULL;
    if (!p) return 0;
    *out = atof(p + strlen(key));
    return 1;
}

int main(int argc, char **argv) {
    int ncol = 9;
    if (argc > 1 && strcmp(argv[1], "-8") == 0) { ncol = 8; argv++; argc--; }
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "usage: %s [-8] frames.f32 out.f32 [in_log out_log]\n", argv[0]);
        return 2;
    }
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    if (!fi || !fo) { perror("open"); return 2; }
    int pcol = ncol == 9 ? 8 : 7;

    bcd_time_detector_t *td = bcd_time_detector_create(NULL);
    bcd_time_detector_set_callback(td, on_bt, NULL);
    float rec[9];
    uint64_t n = 0, dstate = 0, dpulse = 0;
    while (fread(rec, sizeof(float), ncol, fi) == (size_t)ncol) {
        cur_frame = td->frame_count;
        g_pulse = 0.0f;
        td->current_energy = rec[1];
        bcd_time_run_state_machine(td);
        td->frame_count++;
        float start = td->threshold_high;
        if (BCD_TIME_EDGE_FRACTION * td->pulse_level > start) start = BCD_TIME_EDGE_FRACTION * td->pulse_level;
        float out[9] = { (float)((double)(td->frame_count - 1) * FR_MS), td->current_energy,
                         td->noise_floor, td->threshold_high, td->threshold_low, (float)td->state,
                         start, td->pulse_level, g_pulse };
        fwrite(out, sizeof out, 1, fo);
        if ((int)rec[5] != (int)td->state) dstate++;
        if (rec[pcol] != g_pulse) dpulse++;
        n++;
    }
    fclose(fi); fclose(fo);
    fprintf(stderr, "btime_replay: %llu frames, %zu events; frames whose state differs from the "
            "record %llu, whose event width differs %llu\n", (unsigned long long)n, nev,
            (unsigned long long)dstate, (unsigned long long)dpulse);
    bcd_time_detector_destroy(td);

    if (argc == 5) {
        FILE *li = fopen(argv[3], "r"), *lo = fopen(argv[4], "w");
        if (!li || !lo) { perror("open log"); return 2; }
        char line[4096];
        size_t k = 0;
        double pos = -1e300;   /* running max of the log's event times */
        while (fgets(line, sizeof line, li)) {
            double tl;
            int timed = line_time(line, &tl);
            if (strncmp(line, "BTIME ", 6) == 0) continue;
            if (timed && tl > pos) pos = tl;
            /* replayed events reported at or before this line's time go first */
            while (k < nev && timed && evs[k].at <= pos) {
                fprintf(lo, "BTIME t=%.1f dur=%.1f at=%.1f\n", evs[k].t, evs[k].dur, evs[k].at);
                k++;
            }
            fputs(line, lo);
        }
        for (; k < nev; k++)
            fprintf(lo, "BTIME t=%.1f dur=%.1f at=%.1f\n", evs[k].t, evs[k].dur, evs[k].at);
        fclose(li); fclose(lo);
    }
    free(evs);
    return 0;
}
