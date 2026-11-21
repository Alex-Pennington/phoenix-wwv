/* Sync detector holdover and recovery tests.
 *
 * 1. The live 90-minute RSP2 run of 2026-11-20 00:30-02:00 UTC, as the sync
 *    detector saw it: every tick-path (TMARK) and marker-detector (MARK) minute
 *    marker that run reported, at its leading-edge offset from the true :00 and
 *    its report delay, and none in the 44 minutes it missed (at most 4 in a
 *    row). Sync must lock once and hold LOCKED to the end, on the true phase.
 *    On 8c8a477 the same events gave 9 LOCKED -> RECOVERING -> ACQUIRING cycles.
 * 2. Markers stop after lock: LOCKED holds for the 20-minute holdover, then
 *    RECOVERING (not ACQUIRING), and one marker on the held phase re-locks.
 * 3. In RECOVERING, a marker off the held phase (25 s) does not re-lock.
 * 4. With no marker at all, the retention window (60 minutes from the loss)
 *    ends in ACQUIRING.
 * 5. Confidence decays on stream time: one missed minute before the third
 *    marker still locks, at 50 ms or 1000 ms check intervals.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "sync_detector.h"

static int fails;
static void check(int ok, const char *fmt, double a, double b) {
    printf("%-4s ", ok ? "ok" : "FAIL");
    printf(fmt, a, b);
    printf("\n");
    if (!ok) fails++;
}

/* State transitions, with the time of the periodic check or event that made them */
static float g_now;
static int n_lock, n_unlock, n_acq_after_lock;
static float first_lock_ms = -1.0f, last_transition_ms;
static sync_state_t last_state;
static void on_state(sync_state_t o, sync_state_t n, float c, void *u) {
    (void)c; (void)u;
    if (n == SYNC_LOCKED) { n_lock++; if (first_lock_ms < 0) first_lock_ms = g_now; }
    if (o == SYNC_LOCKED) n_unlock++;
    if (n == SYNC_ACQUIRING && first_lock_ms >= 0) n_acq_after_lock++;
    last_state = n;
    last_transition_ms = g_now;
}
static void reset_counts(void) {
    n_lock = n_unlock = n_acq_after_lock = 0;
    first_lock_ms = -1.0f; last_state = SYNC_ACQUIRING; last_transition_ms = 0.0f;
}

/* Live run: minute index from 00:31:00 UTC, TMARK leading-edge offset from :00
 * and report delay (ms), MARK the same; NO = not detected. Generated from the
 * run's wwv_live --log (MARK/TMARK lead= against tb.json's out_utc0_unix). */
#define NO 99999
static const int live[][5] = {
    { 0,   NO,    0,   NO,    0}, { 1,   13,  812,   NO,    0}, { 2,   NO,    0,   NO,    0}, { 3,    9,  813,   NO,    0},
    { 4,   11,  813,   NO,    0}, { 5,   NO,    0,   NO,    0}, { 6,   61,  781,   NO,    0}, { 7,   NO,    0,   NO,    0},
    { 8,   NO,    0,   NO,    0}, { 9,   NO,    0,   NO,    0}, {10,   NO,    0,   50, 1283}, {11,   30,  813,   NO,    0},
    {12,   NO,    0,   NO,    0}, {13,   NO,    0,   NO,    0}, {14,   NO,    0,   NO,    0}, {15,   NO,    0,   NO,    0},
    {16,   16,  813,   NO,    0}, {17,   NO,    0,   NO,    0}, {18,   NO,    0,   NO,    0}, {19,   NO,    0,   NO,    0},
    {20,   NO,    0,  -42, 1373}, {21,   23,  797,   NO,    0}, {22,   NO,    0,   NO,    0}, {23,   NO,    0,   NO,    0},
    {24,   16,  813,   NO,    0}, {25,   NO,    0,   NO,    0}, {26,   NO,    0,   NO,    0}, {27,   NO,    0,   NO,    0},
    {28,   12,  807,   NO,    0}, {29,   NO,    0,   NO,    0}, {30,    8,  813,   NO,    0}, {31,   21,  797,   NO,    0},
    {32,   16,  813,  -45, 1273}, {33,   NO,    0,   NO,    0}, {34,   NO,    0,   NO,    0}, {35,   11,  812,    8, 1429},
    {36,   NO,    0,   NO,    0}, {37,   NO,    0,   -7, 1222}, {38,   NO,    0,   NO,    0}, {39,   11,  807,  -13, 1204},
    {40,   NO,    0,   NO,    0}, {41,   NO,    0,   NO,    0}, {42,   NO,    0,   NO,    0}, {43,   NO,    0,   NO,    0},
    {44,    6,  813,   NO,    0}, {45,   NO,    0,   NO,    0}, {46,   NO,    0,   NO,    0}, {47,   15,  813,    5, 1514},
    {48,   NO,    0,   NO,    0}, {49,   18,  802,   18, 1170}, {50,    8,  813,   NO,    0}, {51,   11,  807,   NO,    0},
    {52,   12,  812,   NO,    0}, {53,   NO,    0,   NO,    0}, {54,   NO,    0,   NO,    0}, {55,   NO,    0,   NO,    0},
    {56,    7,  817,   NO,    0}, {57,   34,  786,   NO,    0}, {58,    9,  812,   NO,    0}, {59,   NO,    0,   NO,    0},
    {60,    7,  812,   NO,    0}, {61,    7,  813,   NO,    0}, {62,   NO,    0,   NO,    0}, {63,   NO,    0,   NO,    0},
    {64,    6,  813,   NO,    0}, {65,    8,  812,   NO,    0}, {66,   NO,    0,   37, 1316}, {67,    9,  813,   NO,    0},
    {68,   NO,    0,   NO,    0}, {69,   NO,    0,   NO,    0}, {70,    9,  812,   NO,    0}, {71,   NO,    0,   NO,    0},
    {72,    7,  812,   56, 1204}, {73,    8,  822,   31, 1516}, {74,    3,  813,   NO,    0}, {75,   NO,    0,   19, 1336},
    {76,   NO,    0,   10, 1454}, {77,   NO,    0,   NO,    0}, {78,    9,  807,   NO,    0}, {79,   NO,    0,   14, 1250},
    {80,   NO,    0,    7, 1360}, {81,   NO,    0,   NO,    0}, {82,   NO,    0,   30, 1386}, {83,   NO,    0,   13, 1393},
    {84,   11,  813,    2, 1298}, {85,   NO,    0,   -4, 1460}, {86,    4,  812,   NO,    0}, {87,   NO,    0,   NO,    0},
    {88,   NO,    0,   NO,    0},
};
#define LIVE_00_MS   58926.5f     /* stream time of 00:31:00 UTC */
#define LIVE_END_MS  5400000.0f   /* 270,000,000 samples at 50 kHz */

typedef struct { float report_ms, lead_ms; int tick_path; } mev_t;
static int cmp_mev(const void *a, const void *b) {
    float d = ((const mev_t *)a)->report_ms - ((const mev_t *)b)->report_ms;
    return (d > 0) - (d < 0);
}

static void run_live(void) {
    mev_t ev[2 * 89]; int n = 0;
    for (size_t i = 0; i < sizeof live / sizeof live[0]; i++) {
        float m00 = LIVE_00_MS + 60000.0f * live[i][0];
        if (live[i][1] != NO) ev[n++] = (mev_t){ m00 + live[i][1] + live[i][2], m00 + live[i][1], 1 };
        if (live[i][3] != NO) ev[n++] = (mev_t){ m00 + live[i][3] + live[i][4], m00 + live[i][3], 0 };
    }
    qsort(ev, n, sizeof ev[0], cmp_mev);
    sync_detector_t *sd = sync_detector_create(NULL);
    sync_detector_set_state_callback(sd, on_state, NULL);
    reset_counts();
    int k = 0, locked_checks = 0, checks_after_lock = 0;
    for (float now = 0.0f; now <= LIVE_END_MS; now += 50.0f) {
        while (k < n && ev[k].report_ms <= now) {
            g_now = ev[k].report_ms;
            if (ev[k].tick_path) sync_detector_tick_marker(sd, ev[k].lead_ms, 800.0f, 1.0f);
            else sync_detector_marker_event(sd, ev[k].lead_ms, 1.0f, 800.0f);
            k++;
        }
        g_now = now;
        sync_detector_periodic_check(sd, now);
        if (first_lock_ms >= 0) {
            checks_after_lock++;
            if (sync_detector_get_state(sd) == SYNC_LOCKED) locked_checks++;
        }
    }
    printf("     live: %d marker events, first lock at %.1f s, %d lock(s), %d unlock(s)\n",
           n, first_lock_ms / 1000.0f, n_lock, n_unlock);
    check(first_lock_ms > 0 && first_lock_ms <= 305000.0f,
          "live: locks by 00:35:05 UTC (first lock %.1f s, limit %.0f s)", first_lock_ms / 1000.0, 305.0);
    check(n_lock == 1 && n_unlock == 0 && n_acq_after_lock == 0,
          "live: never leaves LOCKED once locked (%.0f unlocks, %.0f returns to ACQUIRING)",
          n_unlock, n_acq_after_lock);
    check(locked_checks == checks_after_lock,
          "live: LOCKED in %.0f of %.0f periodic checks after the first lock", locked_checks, checks_after_lock);
    /* the held anchor is on a true :00 */
    float a = sync_detector_get_last_marker_ms(sd);
    float off = fmodf(a - LIVE_00_MS, 60000.0f);
    if (off > 30000.0f) off -= 60000.0f;
    check(fabsf(off) <= 100.0f, "live: anchor %.1f ms from a true :00 (limit %.0f)", off, 100.0);
    sync_detector_destroy(sd);
}

/* Lock on markers at 0.4 + 60k s for minutes 0..3, then run checks to end_ms,
 * delivering the extra marker (if any) at extra_ms. Returns the detector. */
static sync_detector_t *lock_then_silence(float end_ms, float extra_ms, float *state_at_extra_check,
                                          float probe_ms, sync_state_t *probe_state) {
    sync_detector_t *sd = sync_detector_create(NULL);
    sync_detector_set_state_callback(sd, on_state, NULL);
    reset_counts();
    int next = 0; bool extra_done = extra_ms < 0;
    for (float now = 0.0f; now <= end_ms; now += 50.0f) {
        if (next < 4 && now >= 400.0f + 60000.0f * next + 800.0f) {
            g_now = now;
            sync_detector_tick_marker(sd, 400.0f + 60000.0f * next, 800.0f, 1.0f);
            next++;
        }
        if (!extra_done && now >= extra_ms + 800.0f) {
            g_now = now;
            sync_detector_tick_marker(sd, extra_ms, 800.0f, 1.0f);
            extra_done = true;
        }
        g_now = now;
        sync_detector_periodic_check(sd, now);
        if (state_at_extra_check && extra_ms >= 0 && fabsf(now - (extra_ms + 4000.0f)) < 25.0f)
            *state_at_extra_check = (float)sync_detector_get_state(sd);
        if (probe_state && fabsf(now - probe_ms) < 25.0f) *probe_state = sync_detector_get_state(sd);
    }
    return sd;
}

int main(void) {
    run_live();

    /* 2. Last marker at 180.4 s. Holdover ends 20 min later; a marker on the
     *    held phase (+30 ms) at minute 33 (30 min after the last) re-locks. */
    const float last = 180400.0f, hold = 20.0f * 60000.0f;
    sync_state_t at_19 = SYNC_ACQUIRING, at_25 = SYNC_ACQUIRING;
    sync_detector_t *sd = lock_then_silence(last + 19.0f * 60000.0f, -1.0f, NULL,
                                            last + 19.0f * 60000.0f, &at_19);
    check(at_19 == SYNC_LOCKED, "holdover: LOCKED 19 minutes after the last marker (state %.0f, want %.0f)",
          at_19, SYNC_LOCKED);
    sync_detector_destroy(sd);

    float st_after = -1.0f;
    float relock_marker = 400.0f + 60000.0f * 33 + 30.0f;
    sd = lock_then_silence(relock_marker + 10000.0f, relock_marker, &st_after,
                           last + 25.0f * 60000.0f, &at_25);
    check(n_unlock == 1 && at_25 == SYNC_RECOVERING,
          "holdover: one unlock after %.0f min, RECOVERING (not ACQUIRING) at 25 min: state %.0f",
          hold / 60000.0f, at_25);
    check(n_acq_after_lock == 0, "holdover: never returned to ACQUIRING (%.0f times; want %.0f)",
          n_acq_after_lock, 0);
    check(st_after == SYNC_LOCKED && n_lock == 2,
          "recovery: a marker on the held phase re-locks within 4 s (state %.0f, locks %.0f)",
          st_after, n_lock);
    sync_detector_destroy(sd);

    /* 3. A marker 25 s off the held phase in RECOVERING does not re-lock */
    float off_marker = 400.0f + 60000.0f * 33 + 25000.0f;
    st_after = -1.0f;
    sd = lock_then_silence(off_marker + 10000.0f, off_marker, &st_after, 0.0f, NULL);
    check(st_after == SYNC_RECOVERING && n_lock == 1,
          "recovery: a marker 25 s off the held phase does not re-lock (state %.0f, locks %.0f)",
          st_after, n_lock);
    sync_detector_destroy(sd);

    /* 4. No marker: holdover (20 min) then retention (60 min from the loss) */
    sync_state_t at_end = SYNC_LOCKED;
    float end = last + hold + 61.0f * 60000.0f;
    sd = lock_then_silence(end, -1.0f, NULL, end, &at_end);
    check(at_end == SYNC_ACQUIRING && n_acq_after_lock == 1,
          "retention: ACQUIRING 81 minutes after the last marker (state %.0f, resets %.0f)",
          at_end, n_acq_after_lock);
    sync_detector_destroy(sd);

    /* 5. The confidence decay runs on stream time, not per check. Markers at
     *    minutes 0, 1 and 3 (minute 2 missed) lock at the third whatever the
     *    check interval; at 0.9999 per 50 ms check they reached only 0.69. */
    const float step_ms[2] = { 50.0f, 1000.0f };
    float conf_at_end[2];
    for (int k = 0; k < 2; k++) {
        sd = sync_detector_create(NULL);
        sync_detector_set_state_callback(sd, on_state, NULL);
        reset_counts();
        const int minutes[3] = { 0, 1, 3 };
        int next = 0;
        for (float now = 0.0f; now <= 200000.0f; now += step_ms[k]) {
            if (next < 3 && now >= 400.0f + 60000.0f * minutes[next] + 800.0f) {
                g_now = now;
                sync_detector_tick_marker(sd, 400.0f + 60000.0f * minutes[next], 800.0f, 1.0f);
                next++;
            }
            g_now = now;
            sync_detector_periodic_check(sd, now);
        }
        conf_at_end[k] = sync_detector_get_confidence(sd);
        /* Frame time at the last check (200 s): anchor 180.4 s, so second 19 from 199.4 s */
        frame_time_t ft = sync_detector_get_frame_time(sd);
        check(ft.current_second == 19 && fabsf(ft.second_start_ms - 199400.0f) < 1.0f,
              "frame time: second %.0f from %.0f ms at the last check (want 19 from 199400)",
              ft.current_second, ft.second_start_ms);
        check(n_lock == 1 && first_lock_ms >= 180400.0f && first_lock_ms <= 185000.0f,
              "decay: one missed minute, LOCKED at the third marker (%.0f ms, checks every %.0f ms)",
              first_lock_ms, step_ms[k]);
        sync_detector_destroy(sd);
    }
    check(fabsf(conf_at_end[0] - conf_at_end[1]) < 0.01f,
          "decay: same confidence at 50 ms and 1000 ms checks (%.3f / %.3f)", conf_at_end[0], conf_at_end[1]);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
