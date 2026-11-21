/* Tick detector timing gate (tick_detector_set_gating_enabled).
 *
 * Drives tick_state_machine_run() frame by frame with set energies and
 * correlation peaks (tick_internal.h): a background at energy 1, a 2-frame tick
 * at energy 100 (correlation 20) each second, and a 2-frame noise run at energy
 * 100 (correlation 20) at +500 ms. The gate is enabled with its epoch 15 ms
 * after the ticks, as when the sync detector's held phase runs late.
 *
 * Checks: every tick is reported (the gate opens before the epoch), and no
 * noise run is (the gate is shut at +485 ms from the epoch).
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

int main(void) {
    const int seconds = 30, gate_from_s = 3;
    const float fr = tick_detector_get_frame_duration_ms();
    const float late_ms = 15.0f;
    tick_detector_t *td = tick_detector_create(NULL);
    tick_detector_set_callback(td, on_tick, NULL);
    int total = (int)(seconds * 1000.0f / fr), run_left = 0;
    for (int f = 0; f < total; f++) {
        float t = f * fr, in_sec = fmodf(t, 1000.0f);
        if (f == (int)(gate_from_s * 1000.0f / fr)) {
            tick_detector_set_epoch_with_source(td, late_ms, EPOCH_SOURCE_MARKER, 0.9f);
            tick_detector_set_gating_enabled(td, true);
        }
        if (in_sec < fr || (in_sec >= 500.0f && in_sec < 500.0f + fr)) run_left = 2;
        float e = 1.0f;
        if (run_left > 0) { e = 100.0f; run_left--; }
        td->corr_noise_floor = 1.0f;
        td->current_energy = e;
        tick_state_machine_run(td);
        if (td->state == STATE_IN_TICK) td->corr_peak = 20.0f;
        td->frame_count++;
    }
    tick_detector_destroy(td);

    int ticks = 0, noise = 0, want = 0;
    for (int s = gate_from_s + 1; s < seconds; s++) {
        want++;
        for (int i = 0; i < n_ticks && i < 512; i++) {
            float d = tick_t[i] - 1000.0f * s;
            if (d >= 0.0f && d <= 30.0f) ticks++;
            if (d >= 500.0f && d <= 530.0f) noise++;
        }
    }
    int fails = 0;
    printf("%-4s gate epoch %.0f ms late: %d of %d ticks reported\n", ticks == want ? "ok" : "FAIL", late_ms, ticks, want);
    if (ticks != want) fails++;
    printf("%-4s gate shut at +500 ms: %d noise runs reported\n", noise == 0 ? "ok" : "FAIL", noise);
    if (noise) fails++;
    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
