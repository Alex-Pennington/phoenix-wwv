/* Frequency-response checks for the channel filters, measured by driving each
 * filter with a steady sine at fs = 50 kHz and comparing output RMS to input RMS
 * after the transient has decayed. Limits follow the filters' stated design. */
#include <stdio.h>
#include <math.h>
#include "channel_filters.h"

#define FS 50000.0

static double gain_db(int sync, double f) {
    sync_channel_t sc; data_channel_t dc;
    sync_channel_init(&sc); data_channel_init(&dc);
    const int settle = 100000, measure = 100000;  /* 2 s settle, 2 s measure */
    double in2 = 0, out2 = 0;
    for (int n = 0; n < settle + measure; n++) {
        float x = (f == 0.0) ? 1.0f : (float)sin(2 * M_PI * f * n / FS);
        float y = sync ? sync_channel_process(&sc, x) : data_channel_process(&dc, x);
        if (n >= settle) { in2 += (double)x * x; out2 += (double)y * y; }
    }
    return 10.0 * log10((out2 + 1e-30) / in2);
}

static int fails;
static void check(const char *name, int sync, double f, double lo, double hi) {
    double g = gain_db(sync, f);
    int ok = g >= lo && g <= hi;
    printf("%-5s %-26s %6.0f Hz  %8.2f dB  [%g, %g]  %s\n", ok ? "ok" : "FAIL", name, f, g, lo, hi, ok ? "" : "<--");
    if (!ok) fails++;
}

int main(void) {
    /* Data channel: 50-150 Hz bandpass for the 100 Hz BCD subcarrier.
     * The carrier arrives at DC and must be rejected. */
    check("data: carrier at DC", 0, 0.0, -1000, -60);
    check("data: carrier +5 Hz", 0, 5.0, -1000, -60);
    check("data: BCD subcarrier", 0, 100.0, -1.0, 0.5);
    check("data: 500 Hz tone", 0, 500.0, -1000, -40);
    check("data: 1000 Hz tick", 0, 1000.0, -1000, -60);

    /* Sync channel: 800-1400 Hz bandpass for the 1000 Hz tick. The 500/600 Hz
     * tones run 30-40 dB over the floor on air and must be rejected. */
    check("sync: 500 Hz tone", 1, 500.0, -1000, -30);
    check("sync: 600 Hz tone", 1, 600.0, -1000, -20);
    check("sync: 1000 Hz tick", 1, 1000.0, -1.0, 0.5);
    check("sync: 100 Hz BCD", 1, 100.0, -1000, -60);
    check("sync: carrier at DC", 1, 0.0, -1000, -60);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
