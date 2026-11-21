/* BCD time detector: the step fit at low pulse/low contrast (BCD_TIME_FIT_*,
 * include/bcd_time_detector.h).
 *
 * The state machine is driven frame by frame with synthetic bucket energies, as
 * tests/air/btime_replay does with a recorded run, so each case sets exactly what
 * the detector sees. A case is one pulse on a fresh detector:
 *   - the low level is 1.0 times a ripple of period 214 ms and depth +-0.15 (the
 *     100 Hz phase ripple, NOTES.md "The detector's energy ripples"; shallower than
 *     on air, so the low level alone starts no event: without a previous pulse the
 *     detector has no low-level start term), times a noise exp(0.05 g), g Gaussian
 *     from a fixed-seed LCG;
 *   - 3 s of low level; 100 frames before the pulse the noise floor is set to 0.7,
 *     as on air (2 x it is 1.2-1.5 x the low level: NOTES.md, "Saga still starts
 *     events on the low-level subcarrier");
 *   - the rise: 2 or 3 frames, either at the mid level (half-way between the low and
 *     the high level: a slow rise; the true rise is the middle of those frames) or
 *     at 1.05 x the detector's start threshold (a noise peak on the rise, which is
 *     what starts an event at 1.5 dB on air; the true rise is the first of them);
 *   - the high part at the case's contrast (10^(dB/10) times the low level, same
 *     ripple and noise) for 170 / 470 / 770 ms from the rise (0 / 1 / P); in the
 *     1.5-3 dB cases it falls to the low level over +110..+130 ms (4 frames: a
 *     ripple trough or a noise dip, which on air ends the half-amplitude edge early
 *     and reads a 1 or a P as 0);
 *   - 2 s of low level.
 * Contrast is in dB of the bucket value, the frame record's scale. The fit's floor,
 * contrast 1.3, is 1.14 dB, so a clean 1 dB pulse (1.26) is not fitted: the lowest
 * fitted cases are at 1.5 dB.
 *
 * Checks, per case:
 *   - exactly one valid event, and its width classifies (<= 350 ms 0, <= 650 ms 1,
 *     else P: the correlator's cuts) as the case says;
 *   - which width it carries: a fitted width is the box's width (whole frames) plus
 *     BCD_TIME_FIT_OFFSET_MS (15 ms, not a whole number of 5.12 ms frames); an edge
 *     width is whole frames;
 *   - fitted cases: it is reported no later than 176 frames (BCD_TIME_FIT_SPAN_MS)
 *     after the frame that crossed the start threshold; where the case says so, its
 *     start is within 10 ms of the true rise.
 * Cases:
 *   - 0, 1 and P at 1.5 dB with a noise-peak start, and at 2 and 3 dB with a slow
 *     rise: fitted, right class, start within 10 ms;
 *   - 0, 1 and P at 1.5 dB with a slow rise: fitted, right class. KNOWN LIMIT: the
 *     pulse clears the start threshold only on a ripple crest, and the box's rise
 *     can stay there: with other noise seeds the start came out at +41 and +77 ms.
 *     So the start is not checked;
 *   - 0, 1 and P at 7 dB: edge width kept (the fit's ceiling, contrast 4, is 6.0 dB,
 *     so a 5 dB pulse is still fitted; an edge-width case must be over 6 dB);
 *   - a 1 at 1.1 times the low level (0.4 dB), under the fit's floor, with a noise
 *     floor of 0.65 (so the event lives past 100 ms): edge width, and it reads 0;
 *   - a P at 2 dB whose high part sags to the low level for its last 300 ms
 *     (+450..+750 ms): KNOWN MODE, pinned rather than wanted. The box takes the part
 *     before the sag and the P reads 1: the P->1 mechanism of NOTES.md;
 *   - a 1 rising at frame 10 with a noise-peak start, so its start frame is below 40
 *     (BCD_TIME_FIT_PRE_MS of history does not exist yet): edge width. A detector
 *     cannot start an event that early (warmup is 50 frames), so the test marks
 *     warmup done, with a noise floor of 0.8, to reach the guard.
 * Built against a library with the fit disabled (-DBCD_TIME_FIT_CONTRAST_MAX=0.0f),
 * the fitted cases fail: every one carries an edge width, and the 1 and the P at
 * 2 dB and the P at 1.5 dB with a slow rise end at the dip and read 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bcd_time_detector.h"
#include "detection/bcd_internal.h"

#define FR_MS ((double)BCD_TIME_FFT_SIZE * 1000.0 / BCD_TIME_SAMPLE_RATE)
#define SPAN_FRAMES 176
#define RIPPLE 0.15
#define NOISE 0.05
#define NOISE_FLOOR 0.7f
#define BURST 1.05
#ifndef NF_X11
#define NF_X11 0.65f
#endif

enum { SLOW, PEAK };   /* how the pulse rises */

typedef struct {
    const char *name;
    int sym;            /* 0, 1, 2 (P) */
    double db;          /* contrast; < 0: -db is the plain factor */
    int rise_kind, nrise;
    int sag, early;
    int dip;            /* the high part at the low level over +110..+130 ms */
    int want_fit, want_class, check_start;
    float noise_floor;  /* set 100 frames before the rise; 0: NOISE_FLOOR */
} case_t;

typedef struct { double t; float dur; uint64_t report; } ev_t;
static ev_t g_ev[16];
static int g_nev;
static uint64_t g_frame;
static void on_bt(const bcd_time_event_t *e, void *u) {
    (void)u;
    if (g_nev < 16) g_ev[g_nev++] = (ev_t){ e->timestamp_ms, e->duration_ms, g_frame };
}

static unsigned g_rng;
static double gauss(void) {
    double u1, u2;
    g_rng = g_rng * 1103515245u + 12345u; u1 = ((g_rng >> 8) + 1.0) / 16777218.0;
    g_rng = g_rng * 1103515245u + 12345u; u2 = (g_rng >> 8) / 16777216.0;
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}

static int run_case(const case_t *c, unsigned seed) {
    double hi = c->db >= 0 ? pow(10.0, c->db / 10.0) : -c->db;
    double high_ms = c->sym == 0 ? 170 : c->sym == 1 ? 470 : 770;
    long rise = c->early ? 10 : (long)(3000.0 / FR_MS);
    long total = rise + (long)((high_ms + 2000.0) / FR_MS);
    double phase = 2 * M_PI * (seed % 7) / 7.0;
    g_rng = seed; g_nev = 0;
    bcd_time_detector_t *td = bcd_time_detector_create(NULL);
    bcd_time_detector_set_callback(td, on_bt, NULL);
    if (c->early) {   /* as after a warmup on this low level */
        td->warmup_complete = true;
        td->noise_floor = 0.8f;
        td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
        td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
    }
    uint64_t cross[64];
    int ncross = 0;
    for (long f = 0; f < total; f++) {
        double t = f * FR_MS;
        double e = (1.0 + RIPPLE * sin(2 * M_PI * t / 214.0 + phase)) * exp(NOISE * gauss());
        double since = (f - rise) * FR_MS;
        if (f >= rise && since < high_ms) {
            int sagged = (c->sag && since >= 450.0 && since < 750.0) ||
                         (c->dip && since >= 110.0 && since < 130.0);
            if (f < rise + c->nrise)
                e = c->rise_kind == SLOW ? e * 0.5 * (1.0 + hi) : BURST * td->start_threshold;
            else if (!sagged)
                e *= hi;
        }
        if (!c->early && f == rise - 100) {
            td->noise_floor = c->noise_floor > 0.0f ? c->noise_floor : NOISE_FLOOR;
            td->threshold_high = td->noise_floor * BCD_TIME_THRESHOLD_MULT;
            td->threshold_low = td->threshold_high * BCD_TIME_HYSTERESIS_RATIO;
        }
        int was_idle = td->state == STATE_IDLE;
        g_frame = (uint64_t)f;
        td->current_energy = (float)e;
        bcd_time_run_state_machine(td);
        td->frame_count++;
        if (was_idle && td->state == STATE_IN_PULSE && ncross < 64) cross[ncross++] = (uint64_t)f;
    }
    bcd_time_detector_destroy(td);

    if (g_nev != 1) {
        printf("%-14s events=%d: FAIL (want one)\n", c->name, g_nev);
        return 1;
    }
    const ev_t *v = &g_ev[0];
    /* the threshold crossing that started it: a fitted start is at most
     * BCD_TIME_FIT_PRE_MS before it and BCD_TIME_FIT_RISE_MAX_MS after */
    uint64_t c0 = 0;
    for (int k = 0; k < ncross; k++) {
        double ct = cross[k] * FR_MS;
        if (ct >= v->t - BCD_TIME_FIT_RISE_MAX_MS - 0.1 && ct <= v->t + BCD_TIME_FIT_PRE_MS + 0.1 &&
            cross[k] <= v->report) c0 = cross[k];
    }
    int cls = v->dur <= 350 ? 0 : v->dur <= 650 ? 1 : 2;
    double frames = (v->dur - BCD_TIME_FIT_OFFSET_MS) / FR_MS;
    int fitted = fabs(frames - floor(frames + 0.5)) < 1e-3;
    double true_rise = (rise + (c->rise_kind == SLOW ? 0.5 * c->nrise : 0.0)) * FR_MS;
    double start_err = v->t - true_rise;
    long late = (long)v->report - (long)c0;
    int fail = fitted != c->want_fit || cls != c->want_class ||
               (c->want_fit && late > SPAN_FRAMES) ||
               (c->check_start && fabs(start_err) > 10.0);
    printf("%-14s start %+6.1f ms from the rise, width %6.1f ms (%s), class %c, reported %ld frames "
           "after the crossing (frame %llu): %s\n", c->name, start_err, v->dur, fitted ? "fit" : "edge",
           "01P"[cls], late, (unsigned long long)c0, fail ? "FAIL" : "ok");
    return fail;
}

int main(void) {
    static const case_t cases[] = {
        { "0 1.5 dB peak", 0, 1.5, PEAK, 2, 0, 0, 1, 1, 0, 1 },
        { "1 1.5 dB peak", 1, 1.5, PEAK, 3, 0, 0, 1, 1, 1, 1 },
        { "P 1.5 dB peak", 2, 1.5, PEAK, 2, 0, 0, 1, 1, 2, 1 },
        { "0 2 dB slow",   0, 2.0, SLOW, 3, 0, 0, 1, 1, 0, 1 },
        { "1 2 dB slow",   1, 2.0, SLOW, 2, 0, 0, 1, 1, 1, 1 },
        { "P 2 dB slow",   2, 2.0, SLOW, 3, 0, 0, 1, 1, 2, 1 },
        { "0 3 dB slow",   0, 3.0, SLOW, 2, 0, 0, 1, 1, 0, 1 },
        { "1 3 dB slow",   1, 3.0, SLOW, 3, 0, 0, 1, 1, 1, 1 },
        { "P 3 dB slow",   2, 3.0, SLOW, 2, 0, 0, 1, 1, 2, 1 },
        /* KNOWN LIMIT: start not checked (see the header) */
        { "0 1.5 dB slow", 0, 1.5, SLOW, 2, 0, 0, 1, 1, 0, 0 },
        { "1 1.5 dB slow", 1, 1.5, SLOW, 3, 0, 0, 1, 1, 1, 0 },
        { "P 1.5 dB slow", 2, 1.5, SLOW, 2, 0, 0, 1, 1, 2, 0 },
        { "0 7 dB",        0, 7.0, SLOW, 2, 0, 0, 0, 0, 0, 0 },
        { "1 7 dB",        1, 7.0, SLOW, 3, 0, 0, 0, 0, 1, 0 },
        { "P 7 dB",        2, 7.0, SLOW, 2, 0, 0, 0, 0, 2, 0 },
        { "1 x1.1",        1, -1.1, PEAK, 2, 0, 0, 0, 0, 0, 0, NF_X11 },
        /* KNOWN MODE: a P sagging for its last 300 ms reads 1 (see the header) */
        { "P sag 2 dB",    2, 2.0, SLOW, 2, 1, 0, 0, 1, 1, 0 },
        { "1 early",       1, 2.0, PEAK, 2, 0, 1, 0, 0, 1, 0 },
    };
    int fail = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) fail |= run_case(&cases[k], 11u + (unsigned)k);
    printf(fail ? "FAILED\n" : "PASSED\n");
    return fail;
}
