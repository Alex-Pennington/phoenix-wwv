/* Per-frame dump of the BCD time detector, for width forensics.
 *   btime_frames [-n] iq_f32_50k.raw out.f32
 * Runs the harness's data channel and BTIME detector (same wiring as harness_iq.c,
 * -n = waterfall.c slow AGC) and writes one record of 8 float32 per 5.12 ms frame:
 *   t_ms (frame start), energy (the detector's bucket value), noise_floor,
 *   threshold_high, threshold_low, state (0 idle, 1 in pulse, 2 cooldown),
 *   ref (mean |d_i + j d_q|^2 over the 10 ms ending at the frame end: one 100 Hz
 *   period, so free of the subcarrier's phase), pulse (duration_ms of a BTIME event
 *   that ended on this frame, else 0).
 * Analysed by tests/air/btime_width_forensics.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channel_filters.h"
#include "bcd_time_detector.h"
#include "bcd_internal.h"

#define FS 50000.0
#define REF_N 500  /* 10 ms at 50 kHz */

typedef struct { float level; int warmup; } normalizer_t;
static float normalize(normalizer_t *n, float i, float q) {  /* harness_iq.c */
    float mag = sqrtf(i * i + q * q);
    float alpha = (n->warmup < 50000) ? 0.01f : 0.0001f;
    n->level += alpha * (mag - n->level);
    n->warmup++;
    if (n->level < 0.0001f) n->level = 0.0001f;
    return 1.0f / n->level;
}

static float g_pulse;
static void on_bt(const bcd_time_event_t *e, void *u) { (void)u; g_pulse = e->duration_ms; }

int main(int argc, char **argv) {
    int norm = 0;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { norm = 1; argv++; argc--; }
    if (argc < 3) { fprintf(stderr, "usage: %s [-n] iq_f32_50k.raw out.f32\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    if (!fp || !fo) { perror("open"); return 2; }
    normalizer_t nz = {0.01f, 0};
    data_channel_t di, dq; data_channel_init(&di); data_channel_init(&dq);
    bcd_time_detector_t *bt = bcd_time_detector_create(NULL);
    bcd_time_detector_set_callback(bt, on_bt, NULL);
    static float ring[REF_N]; double rsum = 0; int ri = 0;
    static float buf[2 * 8192];
    long n = 0; size_t got;
    while ((got = fread(buf, sizeof(float) * 2, 8192, fp)) > 0) {
        for (size_t k = 0; k < got; k++, n++) {
            float I = buf[2 * k], Q = buf[2 * k + 1];
            if (norm) { float f = normalize(&nz, I, Q); I *= f; Q *= f; }
            float a = data_channel_process(&di, I), b = data_channel_process(&dq, Q);
            float p = a * a + b * b;
            rsum += p - ring[ri]; ring[ri] = p; ri = (ri + 1) % REF_N;
            g_pulse = 0;
            bcd_time_detector_process_sample(bt, a, b);
            if (bt->buffer_idx == 0) {  /* a frame just ran */
                float rec[8] = {
                    (float)((bt->frame_count - 1) * bcd_time_detector_get_frame_duration_ms()),
                    bt->current_energy, bt->noise_floor, bt->threshold_high, bt->threshold_low,
                    (float)bt->state, (float)(rsum / REF_N), g_pulse };
                fwrite(rec, sizeof rec, 1, fo);
            }
        }
    }
    fclose(fp); fclose(fo);
    bcd_time_detector_destroy(bt);
    return 0;
}
