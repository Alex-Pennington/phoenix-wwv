/**
 * @file tick_correlation.c
 * @brief Matched filter correlation for WWV tick detection
 *
 * Implements 5ms complex correlation using cosine/sine templates
 * at the target frequency. Maintains circular buffer and tracks
 * correlation noise floor for threshold decisions.
 */

#include "detection/tick_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*============================================================================
 * Template Generation
 *============================================================================*/

/**
 * Generate complex correlation template (Hann-windowed tone)
 * Template is 5ms of target frequency with smooth windowing
 */
static void generate_template(tick_detector_t *td) {
    for (int i = 0; i < TICK_TEMPLATE_SAMPLES; i++) {
        float t = (float)i / TICK_SAMPLE_RATE;
        /* Hann window for smooth edges */
        float window = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (TICK_TEMPLATE_SAMPLES - 1)));
        /* Complex tone at target frequency */
        td->template_i[i] = cosf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
        td->template_q[i] = sinf(2.0f * M_PI * TICK_TARGET_FREQ_HZ * t) * window;
    }
}

/*============================================================================
 * Correlation Computation
 *============================================================================*/

/**
 * Compute correlation magnitude at current buffer position
 * Returns magnitude of complex correlation
 */
static float compute_correlation(tick_detector_t *td) {
    float sum_i = 0.0f;
    float sum_q = 0.0f;

    /* Correlate template with buffer (complex multiply and accumulate) */
    for (int i = 0; i < TICK_TEMPLATE_SAMPLES; i++) {
        /* Index into circular buffer, starting from oldest sample */
        int buf_idx = (td->corr_buf_idx - TICK_TEMPLATE_SAMPLES + i + TICK_CORR_BUFFER_SIZE) % TICK_CORR_BUFFER_SIZE;

        float sig_i = td->corr_buf_i[buf_idx];
        float sig_q = td->corr_buf_q[buf_idx];
        float tpl_i = td->template_i[i];
        float tpl_q = td->template_q[i];

        /* Complex multiply: (sig_i + j*sig_q) * (tpl_i - j*tpl_q) */
        sum_i += sig_i * tpl_i + sig_q * tpl_q;
        sum_q += sig_q * tpl_i - sig_i * tpl_q;
    }

    return sqrtf(sum_i * sum_i + sum_q * sum_q);
}

/*============================================================================
 * Public Interface (used by tick_detector.c)
 *============================================================================*/

/**
 * Initialize correlation resources
 * Called from tick_detector_create()
 */
void tick_correlation_init(tick_detector_t *td) {
    /* Allocate matched filter resources */
    td->template_i = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->template_q = (float *)malloc(TICK_TEMPLATE_SAMPLES * sizeof(float));
    td->corr_buf_i = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));
    td->corr_buf_q = (float *)malloc(TICK_CORR_BUFFER_SIZE * sizeof(float));

    if (td->template_i && td->template_q && td->corr_buf_i && td->corr_buf_q) {
        /* Initialize matched filter */
        generate_template(td);
        memset(td->corr_buf_i, 0, TICK_CORR_BUFFER_SIZE * sizeof(float));
        memset(td->corr_buf_q, 0, TICK_CORR_BUFFER_SIZE * sizeof(float));
        td->corr_buf_idx = 0;
        td->corr_sample_count = 0;
        td->corr_noise_floor = 0.0f;
    }
}

/**
 * Compute correlation value
 * Called from tick_detector_process_sample() every CORR_DECIMATION samples
 */
float tick_correlation_compute(tick_detector_t *td) {
    return compute_correlation(td);
}
