/**
 * @file iq_frontend.c
 * @brief SDR I/Q front end: carrier to DC, decimation to 50 kHz and 12 kHz
 *
 * See iq_frontend.h. Filters are Kaiser-windowed sinc FIRs designed at create time
 * (Kaiser's formulas for length and beta), normalised to unit DC gain.
 */

#include "iq_frontend.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define FE_MAX_STAGES      8
#define FE_ATTEN_DB        80.0
#define FE_PROTECT_HZ      7000.0   /* decimating stages keep 0..this alias-free */
#define FE_LP_CUTOFF_HZ    6000.0   /* 50 kHz lowpass: -6 dB point */
#define FE_LP_TRANS_HZ     2000.0   /* its transition band (5..7 kHz) */
#define FE_DISP_UP         6        /* 50 kHz * 6 / 25 = 12 kHz */
#define FE_DISP_DOWN       25
#define FE_DISP_CUTOFF_HZ  5800.0
#define FE_DISP_TRANS_HZ   1600.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Complex FIR with decimation; history held twice so the dot product is contiguous */
typedef struct {
    int ntaps;
    int decim;
    float *taps;
    float *hi, *hq;     /* 2 * ntaps */
    int pos;
    int phase;
} fir_t;

typedef struct {
    int ntaps;          /* prototype length, a multiple of FE_DISP_UP */
    int per_phase;
    float *taps;        /* polyphase: taps[p * per_phase + k] = proto[p + k * UP] * UP */
    float *hi, *hq;     /* 2 * per_phase */
    int pos;
    uint64_t n_in;      /* inputs consumed */
    uint64_t next_up;   /* upsampled index of the next output */
} resampler_t;

struct iq_frontend {
    double in_rate;
    float scale;
    double ph_re, ph_im;    /* NCO phasor */
    double w_re, w_im;      /* per-sample rotation */
    unsigned renorm;
    int nstages;
    fir_t stage[FE_MAX_STAGES];
    bool lowpass;
    fir_t lp;
    bool display;
    resampler_t disp;
    double group_delay_s;
    char desc[256];
};

/*============================================================================
 * Filter design
 *============================================================================*/

static double bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

/* Kaiser length for FE_ATTEN_DB with transition trans_hz at rate fs */
static int kaiser_len(double trans_hz, double fs) {
    double dw = 2.0 * M_PI * trans_hz / fs;
    int n = (int)ceil((FE_ATTEN_DB - 7.95) / (2.285 * dw)) + 1;
    if (n < 3) n = 3;
    return n;
}

/* Lowpass prototype, cutoff fc (Hz, -6 dB), at rate fs; DC gain 1 */
static void design_lowpass(float *h, int n, double fc, double fs) {
    double beta = 0.1102 * (FE_ATTEN_DB - 8.7);
    double i0b = bessel_i0(beta);
    double m = (n - 1) / 2.0;
    double sum = 0.0;
    double *tmp = malloc(sizeof(double) * n);
    for (int k = 0; k < n; k++) {
        double x = k - m;
        double s = (fabs(x) < 1e-12) ? 2.0 * fc / fs : sin(2.0 * M_PI * fc / fs * x) / (M_PI * x);
        double r = x / m;
        double w = (m > 0) ? bessel_i0(beta * sqrt(fmax(0.0, 1.0 - r * r))) / i0b : 1.0;
        tmp[k] = s * w;
        sum += tmp[k];
    }
    for (int k = 0; k < n; k++) h[k] = (float)(tmp[k] / sum);
    free(tmp);
}

static bool fir_init(fir_t *f, int ntaps, int decim, double fc, double fs) {
    memset(f, 0, sizeof(*f));
    f->ntaps = ntaps;
    f->decim = decim;
    f->taps = malloc(sizeof(float) * ntaps);
    f->hi = calloc(2 * ntaps, sizeof(float));
    f->hq = calloc(2 * ntaps, sizeof(float));
    if (!f->taps || !f->hi || !f->hq) return false;
    design_lowpass(f->taps, ntaps, fc, fs);
    /* Reverse so the dot product runs over history oldest-first */
    for (int a = 0, b = ntaps - 1; a < b; a++, b--) {
        float t = f->taps[a]; f->taps[a] = f->taps[b]; f->taps[b] = t;
    }
    return true;
}

static void fir_free(fir_t *f) {
    free(f->taps); free(f->hi); free(f->hq);
}

/* Push one sample; returns true and writes the output when one is due */
static inline bool fir_push(fir_t *f, float xi, float xq, float *yi, float *yq) {
    f->hi[f->pos] = f->hi[f->pos + f->ntaps] = xi;
    f->hq[f->pos] = f->hq[f->pos + f->ntaps] = xq;
    f->pos++;
    if (f->pos == f->ntaps) f->pos = 0;
    if (++f->phase < f->decim) return false;
    f->phase = 0;
    /* history oldest..newest is hi[pos .. pos + ntaps - 1] */
    const float *a = f->hi + f->pos, *b = f->hq + f->pos, *h = f->taps;
    float si = 0.0f, sq = 0.0f;
    for (int k = 0; k < f->ntaps; k++) {
        si += h[k] * a[k];
        sq += h[k] * b[k];
    }
    *yi = si;
    *yq = sq;
    return true;
}

static bool resampler_init(resampler_t *r) {
    memset(r, 0, sizeof(*r));
    double fs = (double)IQ_FRONTEND_DETECTOR_RATE * FE_DISP_UP;
    int n = kaiser_len(FE_DISP_TRANS_HZ, fs);
    n = ((n + FE_DISP_UP - 1) / FE_DISP_UP) * FE_DISP_UP;
    r->ntaps = n;
    r->per_phase = n / FE_DISP_UP;
    float *proto = malloc(sizeof(float) * n);
    r->taps = malloc(sizeof(float) * n);
    r->hi = calloc(2 * r->per_phase, sizeof(float));
    r->hq = calloc(2 * r->per_phase, sizeof(float));
    if (!proto || !r->taps || !r->hi || !r->hq) { free(proto); return false; }
    design_lowpass(proto, n, FE_DISP_CUTOFF_HZ, fs);
    /* Phase p, tap k multiplies input x[base - k]; stored newest-last for the
     * oldest-first history walk: taps[p][per_phase - 1 - k] */
    for (int p = 0; p < FE_DISP_UP; p++)
        for (int k = 0; k < r->per_phase; k++)
            r->taps[p * r->per_phase + (r->per_phase - 1 - k)] = proto[p + k * FE_DISP_UP] * FE_DISP_UP;
    free(proto);
    return true;
}

static void resampler_free(resampler_t *r) {
    free(r->taps); free(r->hi); free(r->hq);
}

/* Push one 50 kHz sample; emit every 12 kHz output now due */
static inline void resampler_push(resampler_t *r, float xi, float xq,
                                  iq_frontend_sample_fn cb, void *user) {
    int P = r->per_phase;
    r->hi[r->pos] = r->hi[r->pos + P] = xi;
    r->hq[r->pos] = r->hq[r->pos + P] = xq;
    r->pos++;
    if (r->pos == P) r->pos = 0;
    r->n_in++;
    /* Output j sits at upsampled index j*DOWN; it needs input floor(j*DOWN/UP) */
    while (r->next_up < r->n_in * FE_DISP_UP) {
        int phase = (int)(r->next_up % FE_DISP_UP);
        const float *h = r->taps + phase * P;
        const float *a = r->hi + r->pos, *b = r->hq + r->pos;
        float si = 0.0f, sq = 0.0f;
        for (int k = 0; k < P; k++) {
            si += h[k] * a[k];
            sq += h[k] * b[k];
        }
        if (cb) cb(si, sq, user);
        r->next_up += FE_DISP_DOWN;
    }
}

/*============================================================================
 * Public API
 *============================================================================*/

iq_frontend_t *iq_frontend_create(const iq_frontend_config_t *cfg, char *err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    double ratio = cfg->in_rate / IQ_FRONTEND_DETECTOR_RATE;
    long D = lround(ratio);
    if (D < 1 || fabs(ratio - (double)D) > 1e-9) {
        if (err) snprintf(err, errlen, "input rate %.0f Hz is not a whole multiple of %d Hz",
                          cfg->in_rate, IQ_FRONTEND_DETECTOR_RATE);
        return NULL;
    }

    /* Split D into stages of at most 8, largest first */
    int primes[32], np = 0;
    long rem = D;
    for (long p = 2; p * p <= rem; p++)
        while (rem % p == 0) { primes[np++] = (int)p; rem /= p; }
    if (rem > 1) primes[np++] = (int)rem;
    for (int i = 0; i < np; i++)
        if (primes[i] > 8) {
            if (err) snprintf(err, errlen, "decimation %ld has a prime factor %d > 8", D, primes[i]);
            return NULL;
        }
    int stages[FE_MAX_STAGES], ns = 0;
    for (int i = np - 1; i >= 0; i--) {     /* largest primes first */
        int placed = 0;
        for (int s = 0; s < ns && !placed; s++)
            if (stages[s] * primes[i] <= 8) { stages[s] *= primes[i]; placed = 1; }
        if (!placed) {
            if (ns == FE_MAX_STAGES) {
                if (err) snprintf(err, errlen, "decimation %ld needs too many stages", D);
                return NULL;
            }
            stages[ns++] = primes[i];
        }
    }
    for (int a = 0; a < ns; a++)            /* sort descending */
        for (int b = a + 1; b < ns; b++)
            if (stages[b] > stages[a]) { int t = stages[a]; stages[a] = stages[b]; stages[b] = t; }

    iq_frontend_t *fe = calloc(1, sizeof(*fe));
    if (!fe) return NULL;
    fe->in_rate = cfg->in_rate;
    fe->scale = cfg->scale;
    fe->ph_re = 1.0;
    fe->ph_im = 0.0;
    double w = -2.0 * M_PI * cfg->shift_hz / cfg->in_rate;
    fe->w_re = cos(w);
    fe->w_im = sin(w);
    int off = snprintf(fe->desc, sizeof fe->desc, "%.0f Hz", cfg->in_rate);

    double fs = cfg->in_rate;
    for (int s = 0; s < ns; s++) {
        double fo = fs / stages[s];
        int n = kaiser_len(fo - 2.0 * FE_PROTECT_HZ, fs) | 1;
        if (!fir_init(&fe->stage[s], n, stages[s], fo / 2.0, fs)) { iq_frontend_destroy(fe); return NULL; }
        fe->group_delay_s += (n - 1) / 2.0 / fs;
        off += snprintf(fe->desc + off, sizeof fe->desc - off, " /%d (%d taps)", stages[s], n);
        fs = fo;
    }
    fe->nstages = ns;
    if (D > 1) {
        int n = kaiser_len(FE_LP_TRANS_HZ, fs) | 1;
        if (!fir_init(&fe->lp, n, 1, FE_LP_CUTOFF_HZ, fs)) { iq_frontend_destroy(fe); return NULL; }
        fe->lowpass = true;
        fe->group_delay_s += (n - 1) / 2.0 / fs;
        off += snprintf(fe->desc + off, sizeof fe->desc - off, " lp %.0f Hz (%d taps)", FE_LP_CUTOFF_HZ, n);
    } else {
        off += snprintf(fe->desc + off, sizeof fe->desc - off, " (detector rate: no filter)");
    }
    if (cfg->display) {
        if (!resampler_init(&fe->disp)) { iq_frontend_destroy(fe); return NULL; }
        fe->display = true;
        snprintf(fe->desc + off, sizeof fe->desc - off, ", display x%d/%d (%d taps)",
                 FE_DISP_UP, FE_DISP_DOWN, fe->disp.ntaps);
    }
    return fe;
}

void iq_frontend_destroy(iq_frontend_t *fe) {
    if (!fe) return;
    for (int s = 0; s < FE_MAX_STAGES; s++) fir_free(&fe->stage[s]);
    fir_free(&fe->lp);
    resampler_free(&fe->disp);
    free(fe);
}

double iq_frontend_group_delay_s(const iq_frontend_t *fe) {
    return fe ? fe->group_delay_s : 0.0;
}

const char *iq_frontend_describe(const iq_frontend_t *fe) {
    return fe ? fe->desc : "";
}

void iq_frontend_push(iq_frontend_t *fe, const float *iq, size_t n,
                      iq_frontend_sample_fn det, iq_frontend_sample_fn disp, void *user) {
    for (size_t k = 0; k < n; k++) {
        float xi = iq[2 * k] * fe->scale, xq = iq[2 * k + 1] * fe->scale;

        /* NCO: x * phasor, phasor *= w; renormalised so rounding cannot grow it */
        double pr = fe->ph_re, pi = fe->ph_im;
        float yi = (float)(xi * pr - xq * pi);
        float yq = (float)(xi * pi + xq * pr);
        fe->ph_re = pr * fe->w_re - pi * fe->w_im;
        fe->ph_im = pr * fe->w_im + pi * fe->w_re;
        if (++fe->renorm >= 4096) {
            double m = sqrt(fe->ph_re * fe->ph_re + fe->ph_im * fe->ph_im);
            fe->ph_re /= m;
            fe->ph_im /= m;
            fe->renorm = 0;
        }

        int s = 0;
        for (; s < fe->nstages; s++)
            if (!fir_push(&fe->stage[s], yi, yq, &yi, &yq)) break;
        if (s < fe->nstages) continue;      /* no 50 kHz sample yet */

        if (fe->lowpass) fir_push(&fe->lp, yi, yq, &yi, &yq);
        if (det) det(yi, yq, user);
        if (fe->display) resampler_push(&fe->disp, yi, yq, disp, user);
    }
}
