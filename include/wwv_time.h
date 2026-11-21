/* Stream time: the one type for a time on the detector clock.
 *
 * A stream time is milliseconds since the first detector sample, as a double.
 * Every module that makes, carries or compares one uses wwv_time_ms_t: the tick,
 * marker and BCD detector events, the detector manager and its routing, sync,
 * the correlators and the decoder.
 *
 * Why double ms and not an int64 sample index: every consumer already works in
 * ms and mixes stream times with ms constants and float offsets, so double keeps
 * the arithmetic as it was. A double holds an integer to 2^53, so its step at a
 * stream time t is t * 2^-52: 3e-8 ms at 40 h, 2e-6 ms after a year. A float's
 * step is 1 ms from 2.33 h and 16 ms from 37 h.
 *
 * A duration or an offset inside a second or a minute (a pulse width, an edge's
 * offset from the held phase) stays float: it is small, so its step is small.
 * Take the difference of two stream times in double before narrowing it.
 *
 * A detector stamps an event from its frame count with wwv_frames_to_ms(), which
 * multiplies in integers and divides once in double. The float stamp it replaces,
 * frame_count * a float frame duration, converted the count to float and rounded
 * the product to float, so its error was the float step: 16 ms at 40 h. (The
 * count itself rounds once it passes 2^24 frames, 23.9 h at 5.12 ms; 5.12f is
 * 2.2e-8 off, 3 ms at 40 h, which the step hides.) */
#ifndef WWV_TIME_H
#define WWV_TIME_H

#include <stdint.h>

typedef double wwv_time_ms_t;

/* Stream time of frame `frames` of `frame_samples` samples at `sample_rate` Hz. */
static inline wwv_time_ms_t wwv_frames_to_ms(uint64_t frames, int frame_samples, int sample_rate) {
    return (double)(frames * (uint64_t)frame_samples) * 1000.0 / (double)sample_rate;
}

#endif /* WWV_TIME_H */
