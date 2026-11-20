/* BCD time detector: pulse width on a synthetic WWV 100 Hz time code.
 *
 * The signal is complex baseband at 50 kHz: a carrier at DC, AM by the WWV audio.
 * Each second: a 5 ms 1000 Hz tick at 100% modulation; silence from -10 ms to +30 ms
 * (the protected zone); then the 100 Hz subcarrier at the high level (-15 dBc) for
 * 170/470/770 ms (0/1/P), and at the low level (-30 dBc) for the rest of the second.
 * Sources: docs/WWV Time Signal Generators and Simulators.md (levels, 170/470/770 ms)
 * and docs/DSP filtering for WWV tickBCD separation.md (the 40 ms protected zone).
 * White noise is added to I and Q. Wiring as tests/air/harness_iq.c: data channel
 * (50-150 Hz) -> bcd_time_detector.
 *
 * Checks, per case, after a 10 s settling time:
 *   - every second has exactly one event starting within -100..+250 ms of it (the
 *     correlator's "at the second" band), and no other event;
 *   - its width classifies (correlator cuts: <= 350 ms 0, <= 650 ms 1, else P) as the
 *     symbol sent; and the width is within 60 ms of the sent pulse (200/500/800 ms
 *     from the second, which is where the high level ends);
 *   - no event is carried: none starts between pulses and is still running 60 ms into
 *     the next second, where it would hold that second's pulse. This one is checked in
 *     every second, those after a step and in a fade included.
 * Cases: steady signal; a 12 dB fade for 20 s (40-60 s) and back; a 12 dB rise for
 * 20 s and back. The detector tracks the pulse level for its start threshold, so it
 * needs a few pulses to follow an instant 12 dB step: the STEP_S seconds from each step
 * are reported, not checked. Every other second must pass, the faded and risen ones
 * included (no lock-out).
 * "lowrise": the low level alone rises from -30 to -24 dBc over 40-44 s, holds to 56 s
 * and falls back by 60 s, so the pulse/low step shrinks from 15 to 9 dB while the pulse
 * level holds. Every second is checked: a start threshold tied to the low level must
 * not lose pulses 9 dB over it.
 * "noisefade": a 12 dB fade ramped over 40-44 s, held to 56 s and recovered by 60 s,
 * in noise 25 times the other cases' (sigma 0.1): as the signal fades, the low level
 * plus noise rises toward the start threshold, the RSP2's condition, where it reaches
 * half the tracked pulse level. A start between pulses then runs through the next
 * pulse (its end is half its own mean). Such a carried event fails the case anywhere;
 * the other checks hold outside 40-61 s, and inside it the count of failed seconds is
 * reported. Without the low-level start term (BCD_TIME_START_LOW_RATIO) 4 events carry
 * here, and 1 after the 12 dB step of "fade"; with it, none.
 * The low level between pulses is what defeated the old trailing edge: a threshold
 * tied to the floor under it never sees the pulse end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channel_filters.h"
#include "bcd_time_detector.h"

#define FS 50000.0
#define SECONDS 90
#define SETTLE 10
#define STEP_S 6    /* seconds after each gain step that are reported, not checked */

typedef struct { float t, dur; } ev_t;
static ev_t g_ev[4 * SECONDS];
static int g_nev;
static void on_bt(const bcd_time_event_t *e, void *u) {
    (void)u;
    if (g_nev < 4 * SECONDS) g_ev[g_nev++] = (ev_t){e->timestamp_ms, e->duration_ms};
}

static unsigned g_rng = 1;
static double gauss(void) {  /* Box-Muller on an LCG */
    double u1, u2;
    g_rng = g_rng * 1103515245u + 12345u; u1 = ((g_rng >> 8) + 1.0) / 16777218.0;
    g_rng = g_rng * 1103515245u + 12345u; u2 = (g_rng >> 8) / 16777216.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

/* gain(t): 1, or 'fade' x0.25 / 'rise' x4 for 40 s <= t < 60 s.
 * lo_db_mid: the low level (dBc) at 44-56 s, ramped from and back to -30 dBc over
 * 40-44 s and 56-60 s. */
static int run_case(const char *name, double fade_gain, int ramped, double lo_db_mid, double noise, int verbose) {
    int truth[SECONDS + 1];
    srand(4242);
    for (int s = 0; s <= SECONDS; s++) {
        int sec = s % 60;
        truth[s] = (sec == 0) ? -1 : (sec % 10 == 9) ? 2 : (rand() & 1);
    }
    g_nev = 0; g_rng = 7;
    bcd_time_detector_t *bt = bcd_time_detector_create(NULL);
    bcd_time_detector_set_callback(bt, on_bt, NULL);
    data_channel_t di, dq; data_channel_init(&di); data_channel_init(&dq);
    const double hi = pow(10, -15 / 20.0);
    for (long n = 0; n < (long)(SECONDS * FS); n++) {
        double t = n / FS;
        int s = (int)floor(t);
        double frac = t - s;
        double ramp = t < 40 || t >= 60 ? 0.0 : t < 44 ? (t - 40) / 4 : t < 56 ? 1.0 : (60 - t) / 4;
        double g = ramped ? pow(fade_gain, ramp) : (t >= 40 && t < 60) ? fade_gain : 1.0;
        double lo = pow(10, (-30 + ramp * (lo_db_mid + 30)) / 20.0);
        double a = 0;
        if (frac < 0.005) a += sin(2 * M_PI * 1000 * t);              /* tick */
        int sym = truth[s];
        double hi_end = sym == 0 ? 0.200 : sym == 1 ? 0.500 : sym == 2 ? 0.800 : 0.0;
        if (frac >= 0.030 && frac < 0.990) {                           /* protected zone */
            double lvl = (sym >= 0 && frac < hi_end) ? hi : lo;
            a += lvl * sin(2 * M_PI * 100 * t);
        }
        /* carrier at DC with a fixed phase, so I and Q both carry the sidebands */
        double env = g * (1.0 + a);
        float I = (float)(env * 0.8 + noise * gauss()), Q = (float)(env * 0.6 + noise * gauss());
        bcd_time_detector_process_sample(bt, data_channel_process(&di, I), data_channel_process(&dq, Q));
    }
    bcd_time_detector_destroy(bt);

    int bad = 0, n0 = 0, n01 = 0, n1 = 0, n10 = 0, in_step = 0;
/* A ramped fade's seconds (40-61 s) are reported, not checked, apart from carried events */
#define FADED(sec) (ramped && (sec) >= 40 && (sec) < 62)
#define STEP(sec) (FADED(sec) || fade_gain != 1.0 && !ramped && (((sec) >= 40 && (sec) < 40 + STEP_S) || ((sec) >= 60 && (sec) < 60 + STEP_S)))
    int per_sec[SECONDS + 1];
    memset(per_sec, 0, sizeof per_sec);
    int stray = 0, carried = 0;
    for (int k = 0; k < g_nev; k++) {
        double t = g_ev[k].t / 1000.0;
        int s = (int)floor(t + 0.5);
        double off = (t - s) * 1000.0;
        if (s < SETTLE || s >= SECONDS) continue;
        per_sec[s]++;
        if (off < -100 || off > 250) {
            per_sec[s]--;
            /* started between pulses and still running 60 ms into the next second: it
             * holds that second's pulse, and its end is that pulse's end */
            if (t + g_ev[k].dur / 1000.0 > floor(t) + 1.060) {
                carried++;
                if (verbose) printf("  %s carried event t=%.1f dur=%.0f\n", name, g_ev[k].t, g_ev[k].dur);
            }
            if (STEP(s)) { in_step++; continue; }
            stray++;
            if (verbose) printf("  %s stray event t=%.1f dur=%.0f\n", name, g_ev[k].t, g_ev[k].dur);
            continue;
        }
        float d = g_ev[k].dur;
        int c = d <= 350 ? 0 : d <= 650 ? 1 : 2;
        int sym = truth[s];
        double sent_ms = sym == 0 ? 200 : sym == 1 ? 500 : 800;
        double end_err = off + d - sent_ms;
        int wrong = sym < 0 || c != sym || fabs(end_err) > 60;
        if (STEP(s)) { in_step += wrong; continue; }
        if (sym == 0) { n0++; if (c != 0) n01++; }
        if (sym == 1) { n1++; if (c == 0) n10++; }
        if (wrong) {
            bad++;
            if (verbose) printf("  %s sec %d sent %d: start %+.0f ms width %.0f ms (end %+.0f ms from the sent end)\n",
                                name, s, sym, off, d, end_err);
        }
    }
    int missing = 0, multi = 0;
    for (int s = SETTLE; s < SECONDS; s++) {
        if (STEP(s)) { in_step += (truth[s] < 0) ? (per_sec[s] > 0) : (per_sec[s] != 1); continue; }
        if (truth[s] < 0) { if (per_sec[s]) multi++; continue; }
        if (per_sec[s] == 0) { missing++; if (verbose) printf("  %s sec %d sent %d: no event at the second\n", name, s, truth[s]); }
        if (per_sec[s] > 1) multi++;
    }
    int fail = bad || missing || multi || stray || carried;
    printf("%-6s events=%d sent 0 read 1: %d/%d, sent 1 read 0: %d/%d, wrong width %d, missing %d, extra %d, "
           "stray %d, carried %d; %s (not checked): %d: %s\n",
           name, g_nev, n01, n0, n10, n1, bad, missing, multi, stray, carried,
           ramped ? "in the fade" : "after steps", in_step, fail ? "FAIL" : "ok");
    return fail;
}

int main(int argc, char **argv) {
    int v = argc > 1 && strcmp(argv[1], "-v") == 0;
    int fail = 0;
    if (argc > 4) {   /* -v gain lo_db noise [ramped]: one case, for exploration */
        return run_case("x", atof(argv[2]), argc > 5, atof(argv[3]), atof(argv[4]), v);
    }
    fail |= run_case("steady", 1.0, 0, -30, 0.004, v);
    fail |= run_case("fade", 0.25, 0, -30, 0.004, v);
    fail |= run_case("rise", 4.0, 0, -30, 0.004, v);
    fail |= run_case("lowrise", 1.0, 0, -24, 0.004, v);
    fail |= run_case("noisefade", 0.25, 1, -30, 0.1, v);
    printf(fail ? "FAILED\n" : "PASSED\n");
    return fail;
}
