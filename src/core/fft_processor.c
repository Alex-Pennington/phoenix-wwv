/**
 * @file fft_processor.c
 * @brief Unified FFT processing implementation
 */

#include "fft_processor.h"
#include "external/kiss_fft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct fft_processor {
    /* FFT configuration */
    int fft_size;
    float sample_rate;
    float hz_per_bin;

    /* FFT resources */
    kiss_fft_cfg fft_cfg;
    kiss_fft_cpx *fft_in;
    kiss_fft_cpx *fft_out;
    float *window_func;
};

/*============================================================================
 * Private Functions
 *============================================================================*/

/**
 * Generate Hann window
 */
static void generate_hann_window(float *window, int size) {
    for (int i = 0; i < size; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (size - 1)));
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

fft_processor_t *fft_processor_create(int fft_size, float sample_rate) {
    if (fft_size <= 0 || sample_rate <= 0.0f) {
        return NULL;
    }

    fft_processor_t *fft = (fft_processor_t *)calloc(1, sizeof(fft_processor_t));
    if (!fft) return NULL;

    fft->fft_size = fft_size;
    fft->sample_rate = sample_rate;
    fft->hz_per_bin = sample_rate / fft_size;

    /* Allocate FFT resources */
    fft->fft_cfg = kiss_fft_alloc(fft_size, 0, NULL, NULL);
    if (!fft->fft_cfg) {
        free(fft);
        return NULL;
    }

    fft->fft_in = (kiss_fft_cpx *)malloc(fft_size * sizeof(kiss_fft_cpx));
    fft->fft_out = (kiss_fft_cpx *)malloc(fft_size * sizeof(kiss_fft_cpx));
    fft->window_func = (float *)malloc(fft_size * sizeof(float));

    if (!fft->fft_in || !fft->fft_out || !fft->window_func) {
        fft_processor_destroy(fft);
        return NULL;
    }

    /* Generate window function */
    generate_hann_window(fft->window_func, fft_size);

    return fft;
}

void fft_processor_destroy(fft_processor_t *fft) {
    if (!fft) return;

    if (fft->fft_cfg) free(fft->fft_cfg);
    if (fft->fft_in) free(fft->fft_in);
    if (fft->fft_out) free(fft->fft_out);
    if (fft->window_func) free(fft->window_func);

    free(fft);
}

bool fft_processor_process(fft_processor_t *fft, const float *i_samples, const float *q_samples) {
    if (!fft || !i_samples || !q_samples) {
        return false;
    }

    /* Apply window and load FFT input */
    for (int i = 0; i < fft->fft_size; i++) {
        fft->fft_in[i].r = i_samples[i] * fft->window_func[i];
        fft->fft_in[i].i = q_samples[i] * fft->window_func[i];
    }

    /* Run FFT */
    kiss_fft(fft->fft_cfg, fft->fft_in, fft->fft_out);

    return true;
}

float fft_processor_get_bucket_energy(fft_processor_t *fft, float target_freq, float bandwidth) {
    if (!fft) return 0.0f;

    int center_bin = (int)(target_freq / fft->hz_per_bin + 0.5f);
    int bin_span = (int)(bandwidth / fft->hz_per_bin + 0.5f);
    if (bin_span < 1) bin_span = 1;

    float pos_energy = 0.0f;
    float neg_energy = 0.0f;

    /* Sum energy across positive and negative frequency bins */
    for (int b = -bin_span; b <= bin_span; b++) {
        int pos_bin = center_bin + b;
        int neg_bin = fft->fft_size - center_bin + b;

        if (pos_bin >= 0 && pos_bin < fft->fft_size) {
            float re = fft->fft_out[pos_bin].r;
            float im = fft->fft_out[pos_bin].i;
            pos_energy += sqrtf(re * re + im * im) / fft->fft_size;
        }
        if (neg_bin >= 0 && neg_bin < fft->fft_size) {
            float re = fft->fft_out[neg_bin].r;
            float im = fft->fft_out[neg_bin].i;
            neg_energy += sqrtf(re * re + im * im) / fft->fft_size;
        }
    }

    return pos_energy + neg_energy;
}

float fft_processor_get_hz_per_bin(fft_processor_t *fft) {
    return fft ? fft->hz_per_bin : 0.0f;
}

int fft_processor_get_size(fft_processor_t *fft) {
    return fft ? fft->fft_size : 0;
}

void fft_processor_get_magnitudes(fft_processor_t *fft, float *magnitudes) {
    if (!fft || !magnitudes) return;

    for (int i = 0; i < fft->fft_size; i++) {
        float re = fft->fft_out[i].r;
        float im = fft->fft_out[i].i;
        magnitudes[i] = sqrtf(re * re + im * im);
    }
}
