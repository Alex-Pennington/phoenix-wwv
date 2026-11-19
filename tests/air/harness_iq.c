/* Air-capture harness. Copied from wwv-capture/harness/harness_iq_{,norm_}head.c
 * (a file-input variant of the audit harness) and merged into one program:
 *   harness_iq [-n] iq_f32_50k.raw
 * -n applies the waterfall.c slow AGC before the channel filters (the "norm" wiring).
 * Output lines are identical to the originals so wwv-capture/score.py scores them.
 *
 * Reads interleaved float32 I/Q at exactly 50 kHz (already scaled the way
 * waterfall.c scales int16: /32768) from argv[1] and runs it through the library's
 * own wiring, wwv_detector_manager (channel filters -> tick/marker/BCD time/freq
 * detectors -> sync detector -> BCD correlator -> time decoder). The harness only
 * observes: every event line comes from wwv_chain_log, the manager observer that
 * wwv_live's --log uses too, so the chain scored here is the chain every host runs.
 * Timestamps printed are the detectors' own (ms since sample 0). */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include "wwv_detector_manager.h"
#include "wwv_chain_log.h"
#include "telemetry.h"

#define FS 50000.0
static int PRINT_TELEM = 1;

/* ---- telemetry interposition ---- */
bool telem_init(int port) { (void)port; return true; }
void telem_cleanup(void) {}
void telem_enable(uint32_t c) { (void)c; }
void telem_disable(uint32_t c) { (void)c; }
void telem_set_channels(uint32_t c) { (void)c; }
bool telem_is_enabled(telem_channel_t c) { (void)c; return true; }
void telem_send(telem_channel_t c, const char *l) { if (PRINT_TELEM) printf("TELEM ch=%d %s\n", (int)c, l); }
void telem_sendf(telem_channel_t c, const char *fmt, ...) {
    if (!PRINT_TELEM) return;
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    if (c == TELEM_BCDS) printf("TELEM BCDS %s\n", b);
}
void telem_get_stats(uint32_t *s, uint32_t *d) { if (s) *s = 0; if (d) *d = 0; }
void telem_console(const char *fmt, ...) { (void)fmt; }
void telem_console_flush(void) {}

int main(int argc, char **argv) {
    int norm = 0;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) { norm = 1; argv++; argc--; }
    if (argc < 2) { fprintf(stderr, "usage: %s [-n] iq_f32_50k.raw\n", argv[0]); return 2; }
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) { perror(argv[1]); return 2; }
    wwv_detector_config_t cfg = WWV_DETECTOR_CONFIG_DEFAULT;
    cfg.output_dir = NULL;
    cfg.enable_tone_trackers = false;
    cfg.enable_correlators = false;
    cfg.enable_slow_marker = false;
    cfg.enable_agc = norm;
    cfg.verbose = false;
    wwv_detector_manager_t *mgr = wwv_detector_manager_create(&cfg);
    if (!mgr) { fprintf(stderr, "detector manager create failed\n"); return 2; }
    wwv_chain_log_t *log = wwv_chain_log_create(stdout, mgr);
    const wwv_detector_observer_t obs = wwv_chain_log_observer(log);
    wwv_detector_manager_set_observer(mgr, &obs);
    if (norm) printf("CONFIG NORM=1 FILE=%s FS=%.0f\n", argv[1], FS);
    else printf("CONFIG FILE=%s FS=%.0f\n", argv[1], FS);
    static float buf[2 * 8192];
    long n = 0; size_t got;
    while ((got = fread(buf, sizeof(float) * 2, 8192, fp)) > 0) {
        for (size_t k = 0; k < got; k++, n++)
            wwv_detector_manager_process_detector_sample(mgr, buf[2 * k], buf[2 * k + 1]);
    }
    fclose(fp);
    wwv_chain_log_end(log, n);
    PRINT_TELEM = 1;
    wwv_detector_manager_destroy(mgr);
    wwv_chain_log_destroy(log);
    return 0;
}
