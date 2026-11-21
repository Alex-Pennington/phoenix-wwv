/* Tick detector state machine: a noise run it does not report must not hide the
 * next tick.
 *
 * Drives tick_state_machine_run() frame by frame with set energies and
 * correlation peaks (tick_internal.h), so the run lengths are exact:
 *   - a background at energy 1, correlation floor 1;
 *   - each second a 2-frame tick at energy 100 with correlation 20 (valid);
 *   - 150 ms before a tick, a noise run:
 *     A. every second, 2 frames at energy 10 with correlation 2: fails the
 *        correlation test and is dropped without an event;
 *     B. every other second, 11 frames (56 ms) at energy 20: gap zone, too
 *        short for a marker candidate. The floor reset after it raises the
 *        threshold to 40, and the floor decays back below the noise run's
 *        level in 2 s.
 * On air such runs come about every 530 ms on a weak night (NOTES.md,
 * Detection). With a 500 ms cooldown after them, every tick after one is lost.
 *
 * Checks, per case: no tick event on a noise run, and every tick that follows
 * one (after the warmup) reported.
 */
#include <stdio.h>
#include <math.h>
#include "tick_detector.h"
#include "detection/tick_internal.h"

static int n_ticks;
static float tick_t[512];
static void on_tick(const tick_event_t *e, void *u) {
    (void)u;
    if (n_ticks < 512) tick_t[n_ticks] = e->timestamp_ms;
    n_ticks++;
}

static int run_case(const char *name, int noise_every_s, int noise_frames, float noise_energy, float noise_corr) {
    const int seconds = 40, first_checked = 3;
    const float fr = tick_detector_get_frame_duration_ms();
    tick_detector_t *td = tick_detector_create(NULL);
    tick_detector_set_callback(td, on_tick, NULL);
    n_ticks = 0;
    int total = (int)(seconds * 1000.0f / fr);
    int run_left = 0;
    float run_e = 1.0f, run_c = 0.0f;
    for (int f = 0; f < total; f++) {
        float in_sec = fmodf(f * fr, 1000.0f);
        int sec = (int)(f * fr / 1000.0f);
        if (in_sec < fr) { run_left = 2; run_e = 100.0f; run_c = 20.0f; }       /* tick */
        else if (in_sec >= 850.0f && in_sec < 850.0f + fr &&
                 (sec + 1) % noise_every_s == 0) {                             /* noise run */
            run_left = noise_frames; run_e = noise_energy; run_c = noise_corr;
        }
        float e = 1.0f;
        if (run_left > 0) { e = run_e; run_left--; }
        td->corr_noise_floor = 1.0f;
        td->current_energy = e;
        tick_state_machine_run(td);
        /* The correlation peak is reset on entry to IN_TICK; set it for the run */
        if (td->state == STATE_IN_TICK) td->corr_peak = run_c;
        td->frame_count++;
    }
    tick_detector_destroy(td);

    int on_noise = 0, hit = 0, want = 0;
    for (int s = first_checked; s < seconds; s++) {
        if (s % noise_every_s) continue;
        want++;
        int found = 0;
        for (int i = 0; i < n_ticks && i < 512; i++) {
            float d = tick_t[i] - 1000.0f * s;
            if (d >= 0.0f && d <= 30.0f) found = 1;
            float b = tick_t[i] - (1000.0f * s - 150.0f);
            if (b >= 0.0f && b <= 100.0f) on_noise++;
        }
        hit += found;
    }
    int fails = 0;
    printf("%-4s %s: %d tick events on a noise run\n", on_noise == 0 ? "ok" : "FAIL", name, on_noise);
    if (on_noise) fails++;
    printf("%-4s %s: %d of %d ticks after a noise run reported\n", hit == want ? "ok" : "FAIL", name, hit, want);
    if (hit != want) fails++;
    return fails;
}

int main(void) {
    int fails = 0;
    fails += run_case("A (tick-length, fails correlation)", 1, 2, 10.0f, 2.0f);
    fails += run_case("B (56 ms gap-zone run)", 2, 11, 20.0f, 20.0f);
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
