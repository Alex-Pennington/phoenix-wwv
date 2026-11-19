/**
 * @file iq_frontend.h
 * @brief SDR I/Q front end: shift the carrier to DC and decimate to the detector rates
 *
 * Input: complex samples at in_rate, an integer multiple of 50 kHz, already scaled
 * to float (int16 / 32768, as waterfall.c did). Each sample is multiplied by
 * `scale`, mixed by -shift_hz (a carrier at +shift_hz lands at DC), then decimated:
 *
 *   in_rate --[FIR /d1]--[FIR /d2]...--> 50 kHz --[FIR lowpass 6 kHz]--> detector path
 *                                                 \--[6/25 polyphase]--> 12 kHz display
 *
 * The decimating stages keep 0-7 kHz free of aliases (Kaiser, 80 dB). The 50 kHz
 * lowpass (-6 dB at 6 kHz) gives the detector path the band of the 12 kHz streams
 * the air captures were scored on. At in_rate = 50 kHz nothing is filtered on the
 * detector path: samples pass through scaled and mixed only.
 *
 * Group delay: a detector-path output sample m is the input instant
 * m / 50000 - iq_frontend_group_delay_s() seconds after input sample 0.
 */

#ifndef IQ_FRONTEND_H
#define IQ_FRONTEND_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IQ_FRONTEND_DETECTOR_RATE  50000
#define IQ_FRONTEND_DISPLAY_RATE   12000

typedef struct {
    double in_rate;     /* Hz, an integer multiple of 50000 */
    double shift_hz;    /* carrier offset to remove: +shift_hz moves to 0 Hz */
    float scale;        /* gain applied to the input (1.0 = none) */
    bool display;       /* also produce the 12 kHz display stream */
} iq_frontend_config_t;

typedef struct iq_frontend iq_frontend_t;

typedef void (*iq_frontend_sample_fn)(float i, float q, void *user);

/**
 * Create a front end. On failure returns NULL and writes the reason to err.
 */
iq_frontend_t *iq_frontend_create(const iq_frontend_config_t *cfg, char *err, size_t errlen);
void iq_frontend_destroy(iq_frontend_t *fe);

/**
 * Push n complex samples (interleaved float I/Q). det is called for every 50 kHz
 * output, disp (may be NULL) for every 12 kHz output, in stream order.
 */
void iq_frontend_push(iq_frontend_t *fe, const float *iq, size_t n,
                      iq_frontend_sample_fn det, iq_frontend_sample_fn disp, void *user);

/** Detector-path group delay, seconds */
double iq_frontend_group_delay_s(const iq_frontend_t *fe);

/** One-line description of the stages, e.g. "2000000 /8 (45 taps) /5 (37) lp 6000 Hz (127)" */
const char *iq_frontend_describe(const iq_frontend_t *fe);

#ifdef __cplusplus
}
#endif

#endif /* IQ_FRONTEND_H */
