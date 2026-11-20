#include "channel_filters.h"
#include <string.h>

// Filter coefficients for fs=50000 Hz
// Format: [b0, b1, b2, a0(=1), a1, a2]

// Sync channel: 800-1400 Hz bandpass for the 1000 Hz tick and minute marker.
// 4th-order Butterworth bandpass (scipy.signal.butter(4, [800, 1400], 'bandpass',
// fs=50000, output='sos')), each section scaled to unit gain at 1000 Hz.
// Relative to 1000 Hz: 500 Hz -37.0 dB, 600 Hz -25.9 dB (the WWV 500/600 Hz
// tones), 1200 Hz -0.0 dB. Group delay at 1000 Hz: 1.50 ms.
static const float sync_bp_sos[4][6] = {
    {2.7995046943e-03f, 5.5990093887e-03f, 2.7995046943e-03f, 1.0f, -1.9041931574e+00f, 9.2547172714e-01f},
    {1.9321073444e-03f, 3.8642146887e-03f, 1.9321073444e-03f, 1.0f, -1.9262239124e+00f, 9.3992100265e-01f},
    {9.1236697488e-01f, -1.8247339498e+00f, 9.1236697488e-01f, 1.0f, -1.9353351503e+00f, 9.6450965399e-01f},
    {3.7159593885e-01f, -7.4319187770e-01f, 3.7159593885e-01f, 1.0f, -1.9683111133e+00f, 9.7869700325e-01f}
};

// Data channel: 50-150 Hz bandpass for the 100 Hz BCD subcarrier.
// The SDR delivers the WWV carrier at DC (zero-IF); it sits 20-30 dB above the
// 100 Hz sidebands, so it must be removed before any BCD energy measurement.
// 4th-order Butterworth highpass at 50 Hz (scipy.signal.butter(4, 50, 'highpass',
// fs=50000, output='sos')), each section scaled to unit gain at Nyquist.
// DC: -inf, 20 Hz: -31.8 dB, 100 Hz: -0.0 dB.
static const float data_hp_sos[2][6] = {
    {9.9421882130e-01f, -1.9884376426e+00f, 9.9421882130e-01f, 1.0f, -1.9884180174e+00f, 9.8845726782e-01f},
    {9.9759146654e-01f, -1.9951829331e+00f, 9.9759146654e-01f, 1.0f, -1.9951632413e+00f, 9.9520262488e-01f}
};

// 4th-order Butterworth lowpass at 150 Hz (scipy.signal.butter(4, 150, fs=50000,
// output='sos')), each section scaled to unit DC gain.
// Combined with the highpass: 100 Hz -0.2 dB, 500 Hz -41.8 dB, 1000 Hz -66.0 dB.
static const float data_lp_sos[2][6] = {
    {8.7303533137e-05f, 1.7460706627e-04f, 8.7303533137e-05f, 1.0f, -1.9654195034e+00f, 9.6576871752e-01f},
    {8.8187712892e-05f, 1.7637542578e-04f, 8.8187712892e-05f, 1.0f, -1.9853245871e+00f, 9.8567733795e-01f}
};

// Process one sample through a biquad section
static float biquad_process(biquad_state_t *s, float x, const float *coeffs) {
    float y = coeffs[0] * x + coeffs[1] * s->x1 + coeffs[2] * s->x2
            - coeffs[4] * s->y1 - coeffs[5] * s->y2;
    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;
    return y;
}

// Initialize sync channel (800-1400 Hz bandpass)
void sync_channel_init(sync_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Initialize data channel (50-150 Hz bandpass)
void data_channel_init(data_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Process sample through sync channel (800-1400 Hz bandpass)
float sync_channel_process(sync_channel_t *ch, float x) {
    float y = x;

    for (int k = 0; k < 4; k++) {
        y = biquad_process(&ch->bp[k], y, sync_bp_sos[k]);
    }

    return y;
}

// Process sample through data channel (50-150 Hz bandpass)
float data_channel_process(data_channel_t *ch, float x) {
    float y = x;

    // Highpass cascade (50 Hz): removes the carrier at DC
    y = biquad_process(&ch->hp[0], y, data_hp_sos[0]);
    y = biquad_process(&ch->hp[1], y, data_hp_sos[1]);

    // Lowpass cascade (150 Hz)
    y = biquad_process(&ch->lp[0], y, data_lp_sos[0]);
    y = biquad_process(&ch->lp[1], y, data_lp_sos[1]);

    return y;
}

// Reset sync channel state
void sync_channel_reset(sync_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}

// Reset data channel state
void data_channel_reset(data_channel_t *ch) {
    memset(ch, 0, sizeof(*ch));
}
