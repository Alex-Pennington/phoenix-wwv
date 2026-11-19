/* WWV time decoder: time to CONFIRMED and false confirms on random chains.
 *
 *   sim_wwv_time_decoder [key=value ...]
 *
 * Signal (true rates): p01 (sent 0 read 1), p10 (sent 1 read 0), pP1 (sent P read 1),
 * erase (no symbol at a locked second), mid (a mid-second detection: not evidence),
 * bad (fraction of minutes in a fade) and mult (rate multiplier in a fade, each rate
 * capped at 0.5). chains, minutes, seed. The first minute of every chain starts at
 * a random second, as a capture or a lock does.
 * Model (decoder config; defaults if not given): m01, m10, m01p, m10p, weight, conf
 * (confirmed_nats), looks (confirm_looks).
 * verbose=1 prints every part CONFIRMED wrong to stderr.
 *
 * Prints one line: time of day and date CONFIRMED right, in minutes from the first
 * (partial) minute: median and 90th percentile, and how many chains never did; the
 * chains and minutes with any part CONFIRMED (or PREDICTED) wrong; minutes with a
 * part DECODED wrong. Not a CTest test: a run of thousands of chains takes minutes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "wwv_time_decoder.h"

typedef struct { int year, doy, hour, minute; } utc_t;
static int ylen(int y) { return (y % 4 == 0) ? 366 : 365; }
static utc_t add_minutes(utc_t t, int n) {
    while (n-- > 0) {
        if (++t.minute == 60) { t.minute = 0;
            if (++t.hour == 24) { t.hour = 0;
                if (++t.doy > ylen(t.year)) { t.doy = 1; t.year++; } } }
    }
    return t;
}

static uint64_t rs = 88172645463325252ull;
static double urand(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (double)(rs >> 11) / 9007199254740992.0;
}

typedef struct { int dut1, dst55, dst2, lsw; } status_t;
static utc_t g_truth;
static status_t g_status;
static int g_conf_wrong, g_pred_wrong, g_dec_wrong, g_tod_ok, g_date_ok, g_verbose;
static const wwv_td_result_t *g_r;
static char g_sent[64][61], g_got[64][61];
static int g_nmin, g_dumped;

static void judge(const char *part, wwv_td_state_t st, int ok, float margin) {
    if (st == WWV_TD_NONE || ok) return;
    if (st == WWV_TD_CONFIRMED && g_verbose)
        fprintf(stderr, "CONFIRMED wrong: %s margin %.1f at chain minute %d, truth %04d-%03d %02d:%02d "
                "dut1 %d dst %d%d lsw %d; got %02d:%02d %04d-%03d dut1 %d dst %d%d lsw %d (%s %s %s %s %s)\n",
                part, margin, g_r->chain_minute, g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute,
                g_status.dut1, g_status.dst55, g_status.dst2, g_status.lsw, g_r->hour, g_r->minute,
                g_r->year, g_r->doy, g_r->dut1_tenths, g_r->dst55, g_r->dst2, g_r->lsw,
                wwv_td_state_name(g_r->tod_state), wwv_td_state_name(g_r->date_state),
                wwv_td_state_name(g_r->dut1_state), wwv_td_state_name(g_r->dst_state),
                wwv_td_state_name(g_r->lsw_state));
    if (st == WWV_TD_CONFIRMED && g_verbose > 1 && !g_dumped) {
        g_dumped = 1;
        for (int k = 0; k < g_nmin; k++) fprintf(stderr, "  %2d sent %s\n     got  %s\n", k, g_sent[k], g_got[k]);
    }
    if (st == WWV_TD_CONFIRMED) g_conf_wrong = 1;
    if (st == WWV_TD_PREDICTED) g_pred_wrong = 1;
    if (st == WWV_TD_DECODED) g_dec_wrong = 1;
}

static void on_result(const wwv_td_result_t *r, void *u) {
    (void)u;
    int tod = r->hour == g_truth.hour && r->minute == g_truth.minute;
    int date = r->year == g_truth.year && r->doy == g_truth.doy;
    g_r = r;
    judge("tod", r->tod_state, tod, r->tod_margin);
    judge("date", r->date_state, date, r->date_margin);
    judge("dut1", r->dut1_state, r->dut1_tenths == g_status.dut1, r->dut1_margin);
    judge("dst", r->dst_state, r->dst55 == g_status.dst55 && r->dst2 == g_status.dst2, r->dst_margin);
    judge("lsw", r->lsw_state, r->lsw == g_status.lsw, r->lsw_margin);
    g_tod_ok = r->tod_state == WWV_TD_CONFIRMED && tod;
    g_date_ok = r->date_state == WWV_TD_CONFIRMED && date;
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static void pct(int *v, int n, int cap, char *out, size_t len) {
    qsort(v, (size_t)n, sizeof(int), cmp_int);
    int med = v[n / 2], p90 = v[(int)(0.9 * (n - 1) + 0.5)], never = 0;
    for (int i = 0; i < n; i++) never += v[i] > cap;
    char a[16], b[16];
    if (med > cap) snprintf(a, sizeof a, ">%d", cap); else snprintf(a, sizeof a, "%d", med);
    if (p90 > cap) snprintf(b, sizeof b, ">%d", cap); else snprintf(b, sizeof b, "%d", p90);
    snprintf(out, len, "median %s p90 %s never %d", a, b, never);
}

int main(int argc, char **argv) {
    double p01 = 0.008, p10 = 0.021, pP1 = 0.064, erase = 0.08, mid = 0.07, bad = 0.0, mult = 1.0;
    int chains = 1000, minutes = 30;
    unsigned long long seed = 1;
    wwv_td_config_t cfg;
    wwv_td_default_config(&cfg);
    const char *label = "";
    for (int i = 1; i < argc; i++) {
        char k[32]; double v;
        if (sscanf(argv[i], "%31[^=]=%lf", k, &v) != 2) {
            if (!strncmp(argv[i], "label=", 6)) { label = argv[i] + 6; continue; }
            fprintf(stderr, "bad argument %s\n", argv[i]); return 2;
        }
        if (!strcmp(k, "p01")) p01 = v;
        else if (!strcmp(k, "p10")) p10 = v;
        else if (!strcmp(k, "pP1")) pP1 = v;
        else if (!strcmp(k, "erase")) erase = v;
        else if (!strcmp(k, "mid")) mid = v;
        else if (!strcmp(k, "bad")) bad = v;
        else if (!strcmp(k, "mult")) mult = v;
        else if (!strcmp(k, "chains")) chains = (int)v;
        else if (!strcmp(k, "minutes")) minutes = (int)v;
        else if (!strcmp(k, "seed")) seed = (unsigned long long)v;
        else if (!strcmp(k, "verbose")) g_verbose = (int)v;
        else if (!strcmp(k, "m01")) cfg.p01 = (float)v;
        else if (!strcmp(k, "m10")) cfg.p10 = (float)v;
        else if (!strcmp(k, "m10p")) cfg.p10_pessimistic = (float)v;
        else if (!strcmp(k, "conf")) cfg.confirmed_nats = (float)v;
#ifdef WWV_TD_DEFAULT_CONFIRM_LOOKS
        else if (!strcmp(k, "looks")) cfg.confirm_looks = (int)v;
#endif
#ifdef WWV_TD_DEFAULT_P01_PESSIMISTIC
        else if (!strcmp(k, "m01p")) cfg.p01_pessimistic = (float)v;
        else if (!strcmp(k, "weight")) cfg.p01_prior_weight = (float)v;
#endif
        else { fprintf(stderr, "unknown key %s\n", k); return 2; }
    }
    rs ^= seed * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 10; i++) urand();

    int *t_tod = malloc(sizeof(int) * (size_t)chains), *t_date = malloc(sizeof(int) * (size_t)chains);
    int conf_chains = 0, pred_chains = 0, conf_min = 0, dec_min = 0, total_min = 0;
    for (int c = 0; c < chains; c++) {
        wwv_time_decoder_t *td = wwv_time_decoder_create(&cfg);
        wwv_time_decoder_set_callback(td, on_result, NULL);
        int y = 2000 + (int)(urand() * 100);
        utc_t t0 = {y, 1 + (int)(urand() * ylen(y)), (int)(urand() * 24), (int)(urand() * 60)};
        if (c % 8 == 0) t0 = (utc_t){y, ylen(y), 23, 60 - minutes / 2};   /* across a year end */
        if (t0.year == 2099 && t0.doy > 300) t0.doy = 300;
        g_status = (status_t){(int)(urand() * 15) - 7, urand() < 0.5, 0, urand() < 0.1};
        g_status.dst2 = urand() < 0.9 ? g_status.dst55 : !g_status.dst55;
        int first_sec = 1 + (int)(urand() * 59);
        int any_conf = 0, any_pred = 0;
        g_dumped = 0;
        t_tod[c] = t_date[c] = minutes + 1;
        char f[61];
        for (int k = 0; k < minutes; k++) {
            g_truth = add_minutes(t0, k);
            if (g_truth.year > 2099) break;
            wwv_td_encode(g_truth.year, g_truth.doy, g_truth.hour, g_truth.minute, g_status.dut1,
                          g_status.dst55, g_status.dst2, g_status.lsw, f);
            double m = (urand() < bad) ? mult : 1.0;
            double q01 = fmin(0.5, p01 * m), q10 = fmin(0.5, p10 * m), qP1 = fmin(0.5, pP1 * m);
            float start = 5000.0f + 60000.0f * k;
            g_nmin = k < 64 ? k + 1 : 64;
            if (k < 64) { memcpy(g_sent[k], f, 61); memset(g_got[k], '_', 60); g_got[k][60] = 0; }
            g_conf_wrong = g_pred_wrong = g_dec_wrong = g_tod_ok = g_date_ok = 0;
            for (int s = 0; s < 60; s++) {
                if (k == 0 && s < first_sec) continue;
                if (s == 0 || urand() < erase) {
                    if (k < 64) g_got[k][s] = '.';
                    wwv_time_decoder_symbol(td, start, s, '.', NAN);
                    continue;
                }
                if (urand() < mid) {
                    if (k < 64) g_got[k][s] = '~';
                    wwv_time_decoder_symbol(td, start, s, '0', 520.0f);
                    continue;
                }
                char sym = f[s];
                double u = urand();
                if (sym == '0' && u < q01) sym = '1';
                else if (sym == '1' && u < q10) sym = '0';
                else if (sym == 'P' && u < qP1) sym = '1';
                if (k < 64) g_got[k][s] = sym;
                wwv_time_decoder_symbol(td, start, s, sym, 45.0f);
            }
            wwv_time_decoder_periodic(td, start + 62500.0f);
            total_min++;
            conf_min += g_conf_wrong;
            dec_min += g_dec_wrong;
            any_conf |= g_conf_wrong;
            any_pred |= g_pred_wrong;
            if (g_tod_ok && t_tod[c] > minutes) t_tod[c] = k + 1;
            if (g_date_ok && t_date[c] > minutes) t_date[c] = k + 1;
        }
        conf_chains += any_conf;
        pred_chains += any_pred;
        wwv_time_decoder_destroy(td);
    }
    char a[64], b[64];
    pct(t_tod, chains, minutes, a, sizeof a);
    pct(t_date, chains, minutes, b, sizeof b);
    printf("%s | true p01 %.3f p10 %.3f pP1 %.3f erase %.2f mid %.2f bad %.2f x%.0f | "
           "model %.3f/%.3f pess %.3f/%.3f conf %.2f looks %d | %d chains x %d min | tod CONF: %s | date CONF: %s | "
           "CONFIRMED wrong: %d chains (%.2f per 1000), %d minutes | PREDICTED wrong: %d chains | "
           "DECODED wrong: %d of %d minutes\n",
           label, p01, p10, pP1, erase, mid, bad, mult, cfg.p01, cfg.p10,
#ifdef WWV_TD_DEFAULT_P01_PESSIMISTIC
           cfg.p01_pessimistic,
#else
           cfg.p01,
#endif
           cfg.p10_pessimistic, cfg.confirmed_nats,
#ifdef WWV_TD_DEFAULT_CONFIRM_LOOKS
           cfg.confirm_looks,
#else
           1,
#endif
           chains, minutes, a, b, conf_chains, 1000.0 * conf_chains / chains,
           conf_min, pred_chains, dec_min, total_min);
    free(t_tod);
    free(t_date);
    return conf_chains ? 1 : 0;
}
