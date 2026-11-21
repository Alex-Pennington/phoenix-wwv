/**
 * @file wwv_time_decoder.c
 * @brief WWV/WWVH 100 Hz time code frame decoder (NIST SP 432 Table 3.13)
 *
 * Frame assembly: symbols arrive per second with the start of their minute (the
 * sync anchor). A frame closes when second 59 arrives, when a symbol of a later
 * minute arrives, or 62 s after its start (periodic). Frames whose starts are a
 * whole number of minutes apart (within 1 s) form a chain; anything else starts a
 * new chain.
 *
 * Decode: every hypothesis is a UTC minute from 2000-001 00:00 to 2099-365 23:59
 * for the newest frame; frame k, o_k minutes older, must then carry that minute
 * minus o_k. Each 0/1 symbol whose pulse starts at its second adds its
 * log-likelihood ratio to every hypothesis whose bit there is 1:
 *     observed 1: ln((1 - p10) / p01)      observed 0: ln(p10 / (1 - p01))
 * The search splits into time of day (1440 values, all frames) and date (100 years x
 * 365/366 days), joined where the history crosses 00:00 UTC. Status bits (DUT1,
 * DST, leap second warning) are summed per bit over the frames of the newest
 * frame's UTC day. The search runs twice: under the nominal model, and under the
 * pessimistic one, which trusts both a 1 and a 0 less: its rates are at least
 * p01_pessimistic and p10_pessimistic, and rise with the chain's own reserved
 * seconds and marker seconds (pessimistic_weights()). CONFIRMED needs both to agree.
 */

#include "wwv_time_decoder.h"
#include "telemetry.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define MINUTE_MS           60000.0f
/* A frame chains when its start is a whole number of minutes after the previous
 * one, to strictly less than this. A move of 1 s is a whole second: the symbols
 * may sit in the wrong seconds. 900 ms is sync's PHASE_WRONG_MS, so the decoder
 * chains only moves sync takes as sub-second. On the live replays and the air
 * captures the residuals are under 100 ms in 417 of 433 pairs; the largest that
 * chain with right values are 704 ms (run90g seg2, lost samples) and 530 ms
 * (run3h), and none lies in 705-1045 ms. */
#define CHAIN_TOLERANCE_MS  900.0f
#define SAME_FRAME_MS       30000.0f    /* a symbol within this of the frame start is its */
#define FRAME_CLOSE_MS      62000.0f    /* window 59 closes by start + 61 s (correlator) */
#define MIN_PER_DAY         1440
#define NYEARS              100
#define CHAIN_MARKERS_MIN   2           /* P markers in the history for CONFIRMED */

enum { SYM_MISSING = -2, SYM_ERASED = -1, SYM_ZERO = 0, SYM_ONE = 1, SYM_P = 2 };

static const int P_SECONDS[6] = {9, 19, 29, 39, 49, 59};
static const int RESERVED_SECONDS[15] = {1, 8, 14, 18, 24, 27, 28, 34, 42, 43, 44, 45, 46, 47, 48};
enum { SEC_DST2 = 2, SEC_LSW = 3, SEC_UT1_SIGN = 50, SEC_DST55 = 55, SEC_UT1_MAG = 56 };

/* BCD digit fields, LSB first. weight[i] is the bit of the digit sent at sec[i]. */
typedef struct { int sec[4]; int weight[4]; int n; int max_digit; } field_t;
enum { F_MIN_U, F_MIN_T, F_HOUR_U, F_HOUR_T, F_DOY_U, F_DOY_T, F_DOY_H, F_YEAR_U, F_YEAR_T, NFIELDS };
static const field_t FIELDS[NFIELDS] = {
    [F_MIN_U]  = {{10, 11, 12, 13}, {1, 2, 4, 8}, 4, 9},
    [F_MIN_T]  = {{15, 16, 17},     {1, 2, 4},    3, 5},
    [F_HOUR_U] = {{20, 21, 22, 23}, {1, 2, 4, 8}, 4, 9},
    [F_HOUR_T] = {{25, 26},         {1, 2},       2, 2},
    [F_DOY_U]  = {{30, 31, 32, 33}, {1, 2, 4, 8}, 4, 9},
    [F_DOY_T]  = {{35, 36, 37, 38}, {1, 2, 4, 8}, 4, 9},
    [F_DOY_H]  = {{40, 41},         {1, 2},       2, 3},
    [F_YEAR_U] = {{4, 5, 6, 7},     {1, 2, 4, 8}, 4, 9},
    [F_YEAR_T] = {{51, 52, 53, 54}, {1, 2, 4, 8}, 4, 9},
};

typedef struct {
    float start_ms;
    int index;                  /* chain minute */
    signed char sym[60];
    bool boundary[60];
    float w[2][60];             /* LLR(bit = 1 : bit = 0) from this second's symbol,
                                   under the nominal [0] and pessimistic [1] model */
    signed char obs[60];        /* evidence: +1 a 1, -1 a 0, 0 none (P second, mid-second,
                                   erased, frame not admitted, 1 in an impossible field) */
    float p01[2], p10[2];
    int markers_seen, markers_contradicted, reserved_ones, reserved_zeros, bcd_violations;
    int p_long, p_short;        /* P seconds read at the second as P, and as 1 or 0 */
    bool admitted, hard_valid;
} frame_t;

typedef struct {               /* a CONFIRMED value carried forward in the chain */
    bool valid;
    int index;                  /* chain minute it was confirmed at */
    long value;
    int tod;                    /* time of day then, -1 if not CONFIRMED then */
    int looks;                  /* consecutive minutes this part has met the CONFIRMED
                                   margin under both models, each stepping from the last */
} carried_t;

typedef struct {               /* top two date hypotheses (distinct days) */
    float s1, s2;
    int d1, d2;                 /* yy * 400 + doy, -1 for none */
} date_top_t;

struct wwv_time_decoder {
    wwv_td_config_t cfg;
    frame_t cur;
    bool cur_open;
    int cur_epoch;                          /* phase epoch of the open frame's symbols */
    int cur_resets;                         /* sync phase resets seen with the last symbol */
    frame_t hist[WWV_TD_MAX_FRAMES];
    int nhist;
    wwv_td_result_t last;
    bool have_last;
    carried_t c_tod, c_date, c_dut1, c_dst, c_lsw;
    int conflicts;
    int evidence_frames;                    /* frames in the history with evidence */
    float tod_sum[MIN_PER_DAY];             /* search scratch */
    date_top_t split[WWV_TD_MAX_FRAMES];
    wwv_td_callback_fn cb;
    void *cb_user;
};

/*============================================================================
 * Calendar helpers (2000-2099: a year divisible by 4 is a leap year)
 *============================================================================*/

static int days_in_year(int yy) { return (yy % 4 == 0) ? 366 : 365; }

void wwv_td_doy_to_date(int year, int doy, int *month, int *mday) {
    static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    int m = 0, d = doy;
    while (m < 12) {
        int l = len[m] + (m == 1 && leap);
        if (d <= l) break;
        d -= l;
        m++;
    }
    if (month) *month = m + 1;
    if (mday) *mday = d;
}

/*============================================================================
 * Encoding (NIST SP 432 Table 3.13)
 *============================================================================*/

static void put_digit(char *f, int field, int digit) {
    for (int i = 0; i < FIELDS[field].n; i++)
        f[FIELDS[field].sec[i]] = (digit & FIELDS[field].weight[i]) ? '1' : '0';
}

bool wwv_td_encode(int year, int doy, int hour, int minute, int dut1_tenths,
                   int dst55, int dst2, int lsw, char frame[61]) {
    if (year < 2000 || year > 2099 || doy < 1 || doy > days_in_year(year % 100) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        dut1_tenths < -7 || dut1_tenths > 7)
        return false;
    int yy = year % 100;
    for (int s = 0; s < 60; s++) frame[s] = '0';
    frame[0] = '.';
    frame[60] = '\0';
    for (int i = 0; i < 6; i++) frame[P_SECONDS[i]] = 'P';
    put_digit(frame, F_MIN_U, minute % 10);
    put_digit(frame, F_MIN_T, minute / 10);
    put_digit(frame, F_HOUR_U, hour % 10);
    put_digit(frame, F_HOUR_T, hour / 10);
    put_digit(frame, F_DOY_U, doy % 10);
    put_digit(frame, F_DOY_T, (doy / 10) % 10);
    put_digit(frame, F_DOY_H, doy / 100);
    put_digit(frame, F_YEAR_U, yy % 10);
    put_digit(frame, F_YEAR_T, yy / 10);
    int mag = abs(dut1_tenths);
    frame[SEC_UT1_SIGN] = dut1_tenths >= 0 ? '1' : '0';
    for (int i = 0; i < 3; i++) frame[SEC_UT1_MAG + i] = (mag >> i) & 1 ? '1' : '0';
    frame[SEC_DST55] = dst55 ? '1' : '0';
    frame[SEC_DST2] = dst2 ? '1' : '0';
    frame[SEC_LSW] = lsw ? '1' : '0';
    return true;
}

/*============================================================================
 * Lifecycle
 *============================================================================*/

void wwv_td_default_config(wwv_td_config_t *cfg) {
    cfg->p01 = WWV_TD_DEFAULT_P01;
    cfg->p10 = WWV_TD_DEFAULT_P10;
    cfg->p01_pessimistic = WWV_TD_DEFAULT_P01_PESSIMISTIC;
    cfg->p10_pessimistic = WWV_TD_DEFAULT_P10_PESSIMISTIC;
    cfg->p01_prior_weight = WWV_TD_DEFAULT_P01_PRIOR_WEIGHT;
    cfg->confirm_looks = WWV_TD_DEFAULT_CONFIRM_LOOKS;
    cfg->decoded_nats = WWV_TD_DEFAULT_DECODED;
    cfg->confirmed_nats = WWV_TD_DEFAULT_CONFIRMED;
    cfg->boundary_ms = WWV_TD_DEFAULT_BOUNDARY_MS;
    cfg->max_frames = WWV_TD_MAX_FRAMES;
}

wwv_time_decoder_t *wwv_time_decoder_create(const wwv_td_config_t *cfg) {
    wwv_time_decoder_t *td = (wwv_time_decoder_t *)calloc(1, sizeof(*td));
    if (!td) return NULL;
    if (cfg) td->cfg = *cfg;
    else wwv_td_default_config(&td->cfg);
    if (td->cfg.max_frames < 2) td->cfg.max_frames = 2;
    if (!(td->cfg.p01_prior_weight >= 1.0f)) td->cfg.p01_prior_weight = 1.0f;
    if (td->cfg.confirm_looks < 1) td->cfg.confirm_looks = 1;
    if (td->cfg.max_frames > WWV_TD_MAX_FRAMES) td->cfg.max_frames = WWV_TD_MAX_FRAMES;
    return td;
}

void wwv_time_decoder_destroy(wwv_time_decoder_t *td) { free(td); }

void wwv_time_decoder_set_callback(wwv_time_decoder_t *td, wwv_td_callback_fn cb, void *user_data) {
    if (!td) return;
    td->cb = cb;
    td->cb_user = user_data;
}

const wwv_td_result_t *wwv_time_decoder_get_last(const wwv_time_decoder_t *td) {
    return (td && td->have_last) ? &td->last : NULL;
}

const char *wwv_td_state_name(wwv_td_state_t state) {
    switch (state) {
        case WWV_TD_DECODED:   return "DECODED";
        case WWV_TD_CONFIRMED: return "CONFIRMED";
        case WWV_TD_PREDICTED: return "PREDICTED";
        default:               return "NONE";
    }
}

/*============================================================================
 * Frame checks and evidence
 *============================================================================*/

static bool is_p_second(int s) { return s % 10 == 9; }

static void frame_reset(frame_t *f, float start_ms) {
    memset(f, 0, sizeof(*f));
    f->start_ms = start_ms;
    for (int s = 0; s < 60; s++) f->sym[s] = SYM_MISSING;
}

static void set_weights(frame_t *f, int model) {
    float w1 = logf((1.0f - f->p10[model]) / f->p01[model]);
    float w0 = logf(f->p10[model] / (1.0f - f->p01[model]));
    for (int s = 0; s < 60; s++)
        f->w[model][s] = f->obs[s] > 0 ? w1 : f->obs[s] < 0 ? w0 : 0.0f;
}

/* Integrity checks WWV allows without parity, and the frame's evidence weights. */
static void frame_finish(const wwv_time_decoder_t *td, frame_t *f) {
    for (int i = 0; i < 6; i++) {
        int s = P_SECONDS[i];
        if (f->sym[s] == SYM_P) f->markers_seen++;
        else if (f->sym[s] == SYM_ZERO && f->boundary[s]) f->markers_contradicted++;
    }
    for (int s = 1; s < 60; s++)
        if (!is_p_second(s) && f->sym[s] == SYM_P) f->markers_contradicted++;
    for (int i = 0; i < 6; i++) {
        int s = P_SECONDS[i];
        if (!f->boundary[s]) continue;
        if (f->sym[s] == SYM_P) f->p_long++;
        else if (f->sym[s] == SYM_ZERO || f->sym[s] == SYM_ONE) f->p_short++;
    }
    for (int i = 0; i < 15; i++) {
        int s = RESERVED_SECONDS[i];
        if (!f->boundary[s]) continue;
        if (f->sym[s] == SYM_ONE) f->reserved_ones++;
        else if (f->sym[s] == SYM_ZERO) f->reserved_zeros++;
    }
    /* A frame on the wrong second phase shows 0/1 where the markers are and no P
     * (the correlator labels P only at P seconds), or P elsewhere. A marker at
     * second 0 is the WWVB frame reference: WWV sends no pulse there. */
    f->admitted = f->markers_contradicted <= f->markers_seen && f->sym[0] != SYM_P;

    /* Digits and ranges. The 1s observed in a field are a lower bound on its
     * value; if they already make it impossible, at least one of them is a 0
     * read as 1. Those 1s are not used. */
    bool suspect[NFIELDS] = {false};
    int ones[NFIELDS];
    for (int k = 0; k < NFIELDS; k++) {
        ones[k] = 0;
        for (int i = 0; i < FIELDS[k].n; i++) {
            int s = FIELDS[k].sec[i];
            if (f->boundary[s] && f->sym[s] == SYM_ONE) ones[k] |= FIELDS[k].weight[i];
        }
        if (ones[k] > FIELDS[k].max_digit) suspect[k] = true;
    }
    if (ones[F_HOUR_T] * 10 + ones[F_HOUR_U] > 23) suspect[F_HOUR_T] = suspect[F_HOUR_U] = true;
    if (ones[F_DOY_H] * 100 + ones[F_DOY_T] * 10 + ones[F_DOY_U] > 366)
        suspect[F_DOY_H] = suspect[F_DOY_T] = suspect[F_DOY_U] = true;
    bool drop_one[60] = {false};
    for (int k = 0; k < NFIELDS; k++) {
        if (!suspect[k]) continue;
        f->bcd_violations++;
        for (int i = 0; i < FIELDS[k].n; i++) drop_one[FIELDS[k].sec[i]] = true;
    }

    for (int s = 0; s < 60; s++) {
        f->obs[s] = 0;
        if (!f->admitted || !f->boundary[s] || is_p_second(s)) continue;
        if (f->sym[s] == SYM_ONE && !drop_one[s]) f->obs[s] = 1;
        else if (f->sym[s] == SYM_ZERO) f->obs[s] = -1;
    }

    /* Nominal model: this frame's 0 -> 1 rate, from its reserved bits: they are
     * all sent as 0, so they sample the rate without bias. (An impossible field is
     * chosen for holding an error, so it does not.) The posterior mean under a
     * Beta prior of mean p01 and weight p01_prior_weight. The pessimistic model's
     * rates are set per chain in pessimistic_weights(). */
    float nres = (float)(f->reserved_ones + f->reserved_zeros), W = td->cfg.p01_prior_weight;
    float p01 = ((float)f->reserved_ones + W * td->cfg.p01) / (nres + W);
    if (p01 < 0.001f) p01 = 0.001f;
    if (p01 > 0.9f) p01 = 0.9f;
    f->p01[0] = p01;
    f->p10[0] = td->cfg.p10;
    set_weights(f, 0);

    /* Decodable on its own: every bit observed at its second, reserved bits 0,
     * digits and fields in range, all six markers. Reported, not used. */
    bool ok = f->markers_seen == 6 && f->reserved_ones == 0 && f->reserved_zeros == 15 &&
              f->bcd_violations == 0;
    int digit[NFIELDS];
    for (int k = 0; k < NFIELDS && ok; k++) {
        digit[k] = 0;
        for (int i = 0; i < FIELDS[k].n; i++) {
            int s = FIELDS[k].sec[i];
            if (!f->boundary[s] || (f->sym[s] != SYM_ZERO && f->sym[s] != SYM_ONE)) { ok = false; break; }
            if (f->sym[s] == SYM_ONE) digit[k] |= FIELDS[k].weight[i];
        }
        if (ok && digit[k] > FIELDS[k].max_digit) ok = false;
    }
    static const int status[8] = {SEC_DST2, SEC_LSW, SEC_UT1_SIGN, SEC_DST55, 56, 57, 58, 0};
    for (int i = 0; i < 7 && ok; i++)
        if (!f->boundary[status[i]] || f->sym[status[i]] < SYM_ZERO || f->sym[status[i]] > SYM_ONE) ok = false;
    if (ok) {
        int minute = digit[F_MIN_T] * 10 + digit[F_MIN_U];
        int hour = digit[F_HOUR_T] * 10 + digit[F_HOUR_U];
        int doy = digit[F_DOY_H] * 100 + digit[F_DOY_T] * 10 + digit[F_DOY_U];
        int yy = digit[F_YEAR_T] * 10 + digit[F_YEAR_U];
        ok = minute <= 59 && hour <= 23 && doy >= 1 && doy <= days_in_year(yy);
    }
    f->hard_valid = ok;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Pessimistic model: rates the chain's own evidence supports, never below the
 * configured ones. The 0 -> 1 rate is sampled by the reserved seconds (always sent
 * as 0). The 1 -> 0 rate has no unbiased sample; a P read as 1 or 0 is the same
 * failure (the pulse read short) on a longer pulse, and reads short at least as
 * often as a 1 on the live run and on the air runs with enough errors to compare
 * (tests/air/symbol_rates.py; the header gives the counts). Each rate is
 * pooled over the admitted frames of the history under a Beta prior of the
 * configured rate and weight p01_prior_weight; each frame's rate is then its own
 * counts under a prior of the chain's rate and the same weight, so a frame in a
 * fade is trusted less than the chain around it. Both stay below 0.45, where a 1
 * and a 0 still point opposite ways. Called once per minute: the rates of every
 * frame in the history move with the chain's. */
static void pessimistic_weights(wwv_time_decoder_t *td) {
    float m01 = td->cfg.p01_pessimistic, m10 = td->cfg.p10_pessimistic, W = td->cfg.p01_prior_weight;
    float r1 = 0.0f, rn = 0.0f, ps = 0.0f, pn = 0.0f;
    for (int k = 0; k < td->nhist; k++) {
        const frame_t *f = &td->hist[k];
        if (!f->admitted) continue;
        r1 += (float)f->reserved_ones;
        rn += (float)(f->reserved_ones + f->reserved_zeros);
        ps += (float)f->p_short;
        pn += (float)(f->p_short + f->p_long);
    }
    float c01 = fmaxf(m01, (r1 + W * m01) / (rn + W));
    float c10 = fmaxf(m10, (ps + W * m10) / (pn + W));
    for (int k = 0; k < td->nhist; k++) {
        frame_t *f = &td->hist[k];
        float nres = (float)(f->reserved_ones + f->reserved_zeros);
        float np = (float)(f->p_short + f->p_long);
        f->p01[1] = clampf(fmaxf(m01, ((float)f->reserved_ones + W * c01) / (nres + W)), 0.001f, 0.45f);
        f->p10[1] = clampf(fmaxf(m10, ((float)f->p_short + W * c10) / (np + W)), 0.001f, 0.45f);
        set_weights(f, 1);
    }
}

/*============================================================================
 * Search
 *============================================================================*/

/* Digit score tables for one weight vector: tab[field][digit]. */
static void digit_scores(const float *w, float tab[NFIELDS][16]) {
    for (int k = 0; k < NFIELDS; k++)
        for (int v = 0; v < 16; v++) {
            float s = 0.0f;
            for (int i = 0; i < FIELDS[k].n; i++)
                if (v & FIELDS[k].weight[i]) s += w[FIELDS[k].sec[i]];
            tab[k][v] = s;
        }
}

static float tod_score(float tab[NFIELDS][16], int t) {
    int h = t / 60, m = t % 60;
    return tab[F_MIN_U][m % 10] + tab[F_MIN_T][m / 10] + tab[F_HOUR_U][h % 10] + tab[F_HOUR_T][h / 10];
}

static float doy_score(float tab[NFIELDS][16], int doy) {
    return tab[F_DOY_U][doy % 10] + tab[F_DOY_T][(doy / 10) % 10] + tab[F_DOY_H][doy / 100];
}

static float year_score(float tab[NFIELDS][16], int yy) {
    return tab[F_YEAR_U][yy % 10] + tab[F_YEAR_T][yy / 10];
}

static void top2_add(date_top_t *t, float s, int d) {
    if (s > t->s1) { t->s2 = t->s1; t->d2 = t->d1; t->s1 = s; t->d1 = d; }
    else if (s > t->s2) { t->s2 = s; t->d2 = d; }
}

/* Best two dates for the newest frame. w_today: frames on its UTC day; w_prev:
 * frames on the day before (NULL if none). */
static date_top_t date_search(const float *w_today, const float *w_prev) {
    float t0[NFIELDS][16], t1[NFIELDS][16];
    float doy0[367], doy1[367], yr0[NYEARS], yr1[NYEARS];
    digit_scores(w_today, t0);
    for (int d = 1; d <= 366; d++) doy0[d] = doy_score(t0, d);
    for (int y = 0; y < NYEARS; y++) yr0[y] = year_score(t0, y);
    if (w_prev) {
        digit_scores(w_prev, t1);
        for (int d = 1; d <= 366; d++) doy1[d] = doy_score(t1, d);
        for (int y = 0; y < NYEARS; y++) yr1[y] = year_score(t1, y);
    }
    date_top_t top = {-INFINITY, -INFINITY, -1, -1};
    for (int yy = 0; yy < NYEARS; yy++) {
        int n = days_in_year(yy);
        for (int doy = 1; doy <= n; doy++) {
            float s = doy0[doy] + yr0[yy];
            if (w_prev) {
                /* 2000-001's previous day is sent as year 99, day 365 */
                int py = doy > 1 ? yy : (yy + NYEARS - 1) % NYEARS;
                int pd = doy > 1 ? doy - 1 : days_in_year(py);
                s += doy1[pd] + yr1[py];
            }
            top2_add(&top, s, yy * 400 + doy);
        }
    }
    return top;
}

typedef struct {
    int tod, yy, doy;
    float tod_margin, date_margin;
    long dut1, dst, lsw;        /* dst = second 55 * 2 + second 2 */
    float dut1_margin, dst_margin, lsw_margin;
    int observations;
} search_t;

/* Status bits: each bit's LLR summed over the newest frame's UTC day. A part's
 * margin is its least certain bit; the UT1 sign does not count when the
 * magnitude is 0. */
static void status_values(const float *L, search_t *r) {
    int mag = (L[56] > 0) | ((L[57] > 0) << 1) | ((L[58] > 0) << 2);
    r->dut1_margin = fminf(fabsf(L[56]), fminf(fabsf(L[57]), fabsf(L[58])));
    if (mag) r->dut1_margin = fminf(r->dut1_margin, fabsf(L[SEC_UT1_SIGN]));
    r->dut1 = (L[SEC_UT1_SIGN] > 0 || mag == 0) ? mag : -mag;
    r->dst = (L[SEC_DST55] > 0) * 2 + (L[SEC_DST2] > 0);
    r->dst_margin = fminf(fabsf(L[SEC_DST55]), fabsf(L[SEC_DST2]));
    r->lsw = L[SEC_LSW] > 0;
    r->lsw_margin = fabsf(L[SEC_LSW]);
}

static void search(wwv_time_decoder_t *td, int model, search_t *r) {
    const frame_t *newest = &td->hist[td->nhist - 1];
    int maxo = 0;
    r->observations = 0;
    float *tod_sum = td->tod_sum;
    memset(tod_sum, 0, sizeof(td->tod_sum));
    float w_all[60] = {0};
    for (int k = 0; k < td->nhist; k++) {
        const frame_t *f = &td->hist[k];
        int o = newest->index - f->index;
        if (o > maxo) maxo = o;
        float tab[NFIELDS][16];
        digit_scores(f->w[model], tab);
        for (int t = 0; t < MIN_PER_DAY; t++)
            tod_sum[t] += tod_score(tab, ((t - o) % MIN_PER_DAY + MIN_PER_DAY) % MIN_PER_DAY);
        for (int s = 0; s < 60; s++) {
            w_all[s] += f->w[model][s];
            if (f->w[model][s] != 0.0f) r->observations++;
        }
    }

    /* Dates: all frames on one day when the newest time of day is >= maxo. For
     * earlier times of day the older frames fall on the previous day. */
    date_top_t day_all = date_search(w_all, NULL);
    date_top_t *split = td->split;
    for (int t = 0; t < maxo && t < MIN_PER_DAY; t++) {
        float w0[60] = {0}, w1[60] = {0};
        for (int k = 0; k < td->nhist; k++) {
            const frame_t *f = &td->hist[k];
            float *dst = (newest->index - f->index > t) ? w1 : w0;
            for (int s = 0; s < 60; s++) dst[s] += f->w[model][s];
        }
        split[t] = date_search(w0, w1);
    }

    float best = -INFINITY;
    int bt = 0, bd = -1;
    for (int t = 0; t < MIN_PER_DAY; t++) {
        const date_top_t *d = (t < maxo) ? &split[t] : &day_all;
        float s = tod_sum[t] + d->s1;
        if (s > best) { best = s; bt = t; bd = d->d1; }
    }
    float second_tod = -INFINITY, second_date = -INFINITY;
    for (int t = 0; t < MIN_PER_DAY; t++) {
        const date_top_t *d = (t < maxo) ? &split[t] : &day_all;
        if (t != bt && tod_sum[t] + d->s1 > second_tod) second_tod = tod_sum[t] + d->s1;
        float alt = (d->d1 != bd) ? d->s1 : d->s2;
        if (tod_sum[t] + alt > second_date) second_date = tod_sum[t] + alt;
    }
    r->tod = bt;
    r->yy = bd / 400;
    r->doy = bd % 400;
    r->tod_margin = best - second_tod;
    r->date_margin = best - second_date;

    /* Status bits: frames on the newest frame's UTC day */
    float L[60] = {0};
    for (int k = 0; k < td->nhist; k++) {
        const frame_t *f = &td->hist[k];
        if (newest->index - f->index > bt) continue;
        for (int s = 0; s < 60; s++) L[s] += f->w[model][s];
    }
    status_values(L, r);
}

/*============================================================================
 * States
 *============================================================================*/

/* One part of the result. prev_ok: the previous frame in the chain had this part
 * DECODED or better with the value this one steps from. predicted: the carried
 * CONFIRMED value at this minute, when there is one. Returns the state and sets
 * *value to what is reported. */
static wwv_td_state_t part_state(wwv_time_decoder_t *td, float margin, long decoded,
                                 bool prev_ok, bool markers_ok, carried_t *carried,
                                 bool have_prediction, long predicted, long *value,
                                 int index, int tod_confirmed) {
    /* One frame cannot be checked against another: a reading needs evidence
     * from two frames of the chain. */
    bool decoded_ok = margin >= td->cfg.decoded_nats && td->evidence_frames >= 2;
    bool strong = decoded_ok && margin >= td->cfg.confirmed_nats;
    /* The margin is looked at every minute, and over many looks a wrong value can
     * lead once by chance; it must lead on confirm_looks consecutive minutes. */
    bool qualifies = strong && markers_ok;
    carried->looks = qualifies ? ((prev_ok && carried->looks > 0) ? carried->looks + 1 : 1) : 0;
    if (decoded_ok && carried->valid && have_prediction && decoded != predicted) {
        td->conflicts++;
        carried->valid = false;
        *value = decoded;
        return WWV_TD_DECODED;
    }
    if (qualifies && prev_ok && carried->looks >= td->cfg.confirm_looks) {
        carried->valid = true;
        carried->index = index;
        carried->value = decoded;
        carried->tod = tod_confirmed;
        *value = decoded;
        return WWV_TD_CONFIRMED;
    }
    if (carried->valid && have_prediction) {
        *value = predicted;
        return WWV_TD_PREDICTED;
    }
    if (decoded_ok) {
        *value = decoded;
        return WWV_TD_DECODED;
    }
    *value = -1;
    return WWV_TD_NONE;
}

static bool decoded_or_better(wwv_td_state_t s) {
    return s == WWV_TD_DECODED || s == WWV_TD_CONFIRMED;
}

static long next_day(long d) {  /* d = yy * 400 + doy */
    int yy = (int)(d / 400), doy = (int)(d % 400);
    if (doy < days_in_year(yy)) return d + 1;
    return (long)((yy + 1) % NYEARS) * 400 + 1;
}

/* Whole days crossed between a carried value and now, or -1 if not known (the
 * time of day was not CONFIRMED when the value was). */
static int days_crossed(const carried_t *c, int index) {
    if (c->tod < 0) return -1;
    return (c->tod + (index - c->index)) / MIN_PER_DAY;
}

/* A status part (DUT1, DST bits, leap second warning): constant within a UTC day. */
static wwv_td_state_t status_part(wwv_time_decoder_t *td, carried_t *c, long value, float margin,
                                  bool prev_in_chain, wwv_td_state_t prev_state, long prev_value,
                                  bool markers_ok, int index, int tod_now_confirmed, long *out) {
    bool prev_ok = prev_in_chain && decoded_or_better(prev_state) && prev_value == value;
    bool have_pred = c->valid && days_crossed(c, index) == 0;
    return part_state(td, margin, value, prev_ok, markers_ok, c, have_pred, c->value, out,
                      index, tod_now_confirmed);
}

/* r: nominal model. rp: pessimistic model. A part may be CONFIRMED only if the
 * pessimistic model gives the same value with the same margin requirement, so
 * the result does not rest on how far a 0 or a 1 is trusted. */
static void decide(wwv_time_decoder_t *td, const search_t *r, const search_t *rp,
                   wwv_td_result_t *out) {
    float conf = td->cfg.confirmed_nats;
    const frame_t *newest = &td->hist[td->nhist - 1];
    const wwv_td_result_t *prev = td->have_last ? &td->last : NULL;
    int index = newest->index;
    int n = prev ? index - prev->chain_minute : 0;
    bool prev_in_chain = prev && td->nhist >= 2 && n >= 1 &&
                         td->hist[td->nhist - 2].index == prev->chain_minute;
    int markers = 0;
    td->evidence_frames = 0;
    for (int k = 0; k < td->nhist; k++) {
        if (!td->hist[k].admitted) continue;
        markers += td->hist[k].markers_seen;
        for (int s = 0; s < 60; s++)
            if (td->hist[k].w[0][s] != 0.0f) { td->evidence_frames++; break; }
    }
    bool markers_ok = markers >= CHAIN_MARKERS_MIN;
    long v;

    /* Time of day */
    bool tod_prev_ok = prev_in_chain && decoded_or_better(prev->tod_state) &&
                       (prev->hour * 60 + prev->minute + n) % MIN_PER_DAY == r->tod;
    long tod_pred = td->c_tod.valid ? (td->c_tod.value + (index - td->c_tod.index)) % MIN_PER_DAY : -1;
    bool tod_robust = rp->tod == r->tod && rp->tod_margin >= conf;
    out->tod_state = part_state(td, r->tod_margin, r->tod, tod_prev_ok, markers_ok && tod_robust, &td->c_tod,
                                td->c_tod.valid, tod_pred, &v, index, -1);
    if (out->tod_state == WWV_TD_CONFIRMED) td->c_tod.tod = r->tod;
    out->tod_margin = r->tod_margin;
    out->hour = v >= 0 ? (int)(v / 60) : -1;
    out->minute = v >= 0 ? (int)(v % 60) : -1;
    int tod_now_confirmed = out->tod_state == WWV_TD_CONFIRMED ? r->tod : -1;
    int tod_known = (out->tod_state != WWV_TD_NONE) ? (int)v : -1;
    /* Date and status bits mean something only in frames that decode as WWV time:
     * they are CONFIRMED only while the time of day is reported. */
    bool frames_ok = markers_ok && tod_known >= 0;

    /* Date */
    long day = (long)r->yy * 400 + r->doy;
    bool date_prev_ok = false;
    if (prev_in_chain && decoded_or_better(prev->date_state)) {
        long pd = (long)(prev->year % 100) * 400 + prev->doy;
        if (pd == day) date_prev_ok = true;
        /* the next day, when this minute's time of day shows 00:00 was crossed */
        else if (next_day(pd) == day && tod_known >= 0 && tod_known < n) date_prev_ok = true;
    }
    int crossed = days_crossed(&td->c_date, index);
    long date_pred = td->c_date.valid ? td->c_date.value : -1;
    if (crossed > 0) date_pred = next_day(date_pred);
    bool date_have_pred = td->c_date.valid && crossed >= 0 && crossed <= 1;
    bool date_robust = rp->yy == r->yy && rp->doy == r->doy && rp->date_margin >= conf;
    out->date_state = part_state(td, r->date_margin, day, date_prev_ok, frames_ok && date_robust, &td->c_date,
                                 date_have_pred, date_pred, &v, index, tod_now_confirmed);
    out->date_margin = r->date_margin;
    if (v >= 0) {
        out->year = 2000 + (int)(v / 400);
        out->doy = (int)(v % 400);
        wwv_td_doy_to_date(out->year, out->doy, &out->month, &out->mday);
    } else {
        out->year = out->doy = out->month = out->mday = -1;
    }

    /* Status bits. They change at 00:00 UTC at most, so a carried value holds
     * only while 00:00 is known not to have been crossed. */
    long pv_dut1 = prev ? prev->dut1_tenths : 0, pv_dst = prev ? prev->dst55 * 2 + prev->dst2 : 0;
    long pv_lsw = prev ? prev->lsw : 0;
    long vals[3];
    out->dut1_margin = r->dut1_margin;
    out->dut1_state = status_part(td, &td->c_dut1, r->dut1, r->dut1_margin, prev_in_chain,
                                  prev ? prev->dut1_state : WWV_TD_NONE, pv_dut1,
                                  frames_ok && rp->dut1 == r->dut1 && rp->dut1_margin >= conf,
                                  index, tod_now_confirmed, &vals[0]);
    out->dst_margin = r->dst_margin;
    out->dst_state = status_part(td, &td->c_dst, r->dst, r->dst_margin, prev_in_chain,
                                 prev ? prev->dst_state : WWV_TD_NONE, pv_dst,
                                 frames_ok && rp->dst == r->dst && rp->dst_margin >= conf,
                                 index, tod_now_confirmed, &vals[1]);
    out->lsw_margin = r->lsw_margin;
    out->lsw_state = status_part(td, &td->c_lsw, r->lsw, r->lsw_margin, prev_in_chain,
                                 prev ? prev->lsw_state : WWV_TD_NONE, pv_lsw,
                                 frames_ok && rp->lsw == r->lsw && rp->lsw_margin >= conf,
                                 index, tod_now_confirmed, &vals[2]);
    out->dut1_tenths = (int)vals[0];
    out->dst55 = vals[1] >= 0 ? (int)(vals[1] >> 1) : -1;
    out->dst2 = vals[1] >= 0 ? (int)(vals[1] & 1) : -1;
    out->lsw = (int)vals[2];
}

/*============================================================================
 * Frame close
 *============================================================================*/

static void reset_chain(wwv_time_decoder_t *td) {
    td->nhist = 0;
    td->c_tod.valid = td->c_date.valid = td->c_dut1.valid = td->c_dst.valid = td->c_lsw.valid = false;
    td->c_tod.looks = td->c_date.looks = td->c_dut1.looks = td->c_dst.looks = td->c_lsw.looks = 0;
    td->have_last = false;
}

static void emit(const wwv_time_decoder_t *td, const wwv_td_result_t *r) {
    telem_sendf(TELEM_BCDS, "FRAME,%.1f,%d,%s,%d,%d,%d,%d,%d,%.2f,%d,%d",
                r->frame_start_ms, r->chain_minute, r->frame, r->markers_seen,
                r->markers_contradicted, r->reserved_ones, r->reserved_zeros, r->bcd_violations, r->p01,
                r->admitted ? 1 : 0, r->hard_valid ? 1 : 0);
    char tod[8] = "--:--", date[16] = "----------", doy[8] = "---", dut1[8] = "--", dst[4] = "--",
         lsw[4] = "-";
    if (r->tod_state != WWV_TD_NONE) snprintf(tod, sizeof tod, "%02d:%02d", r->hour, r->minute);
    if (r->date_state != WWV_TD_NONE) {
        snprintf(date, sizeof date, "%04d-%02d-%02d", r->year, r->month, r->mday);
        snprintf(doy, sizeof doy, "%03d", r->doy);
    }
    if (r->dut1_state != WWV_TD_NONE) snprintf(dut1, sizeof dut1, "%+.1f", r->dut1_tenths / 10.0);
    if (r->dst_state != WWV_TD_NONE) snprintf(dst, sizeof dst, "%d%d", r->dst55, r->dst2);
    if (r->lsw_state != WWV_TD_NONE) snprintf(lsw, sizeof lsw, "%d", r->lsw);
    telem_sendf(TELEM_BCDS, "UTC,%.1f,%s,%s,%.1f,%s,%s,%s,%.1f,%s,%s,%.1f,%s,%s,%.1f,%s,%s,%.1f,%d,%d,%d",
                r->frame_start_ms,
                wwv_td_state_name(r->tod_state), tod, r->tod_margin,
                wwv_td_state_name(r->date_state), date, doy, r->date_margin,
                wwv_td_state_name(r->dut1_state), dut1, r->dut1_margin,
                wwv_td_state_name(r->dst_state), dst, r->dst_margin,
                wwv_td_state_name(r->lsw_state), lsw, r->lsw_margin,
                r->chain_frames, r->observations, r->conflicts);
    (void)td;
}

static void close_frame(wwv_time_decoder_t *td) {
    if (!td->cur_open) return;
    td->cur_open = false;
    frame_t *f = &td->cur;
    frame_finish(td, f);

    /* Chain: a whole number of minutes after the previous frame */
    if (td->nhist > 0) {
        const frame_t *p = &td->hist[td->nhist - 1];
        float d = f->start_ms - p->start_ms;
        int n = (int)lroundf(d / MINUTE_MS);
        if (n >= 1 && fabsf(d - n * MINUTE_MS) < CHAIN_TOLERANCE_MS) f->index = p->index + n;
        else reset_chain(td);
    }
    if (td->nhist == 0) f->index = 0;
    /* Keep max_frames minutes of history */
    int keep_from = 0;
    while (keep_from < td->nhist && td->hist[keep_from].index <= f->index - td->cfg.max_frames) keep_from++;
    if (keep_from > 0) {
        memmove(td->hist, td->hist + keep_from, (size_t)(td->nhist - keep_from) * sizeof(frame_t));
        td->nhist -= keep_from;
    }
    if (td->nhist == WWV_TD_MAX_FRAMES) {   /* cannot happen: indices are distinct */
        memmove(td->hist, td->hist + 1, (size_t)(td->nhist - 1) * sizeof(frame_t));
        td->nhist--;
    }
    td->hist[td->nhist++] = *f;

    search_t r, rp;
    search(td, 0, &r);
    pessimistic_weights(td);
    search(td, 1, &rp);
    wwv_td_result_t out;
    memset(&out, 0, sizeof(out));
    out.frame_start_ms = f->start_ms;
    out.chain_minute = f->index;
    out.chain_frames = td->nhist;
    out.observations = r.observations;
    for (int s = 0; s < 60; s++) {
        char c;
        switch (f->sym[s]) {
            case SYM_MISSING: c = '_'; break;
            case SYM_ERASED:  c = '.'; break;
            case SYM_P:       c = 'P'; break;
            default:          c = f->boundary[s] ? (char)('0' + f->sym[s]) : '~'; break;
        }
        out.frame[s] = c;
    }
    out.frame[60] = '\0';
    out.markers_seen = f->markers_seen;
    out.markers_contradicted = f->markers_contradicted;
    out.reserved_ones = f->reserved_ones;
    out.reserved_zeros = f->reserved_zeros;
    out.bcd_violations = f->bcd_violations;
    out.p01 = f->p01[0];
    out.admitted = f->admitted;
    out.hard_valid = f->hard_valid;
    decide(td, &r, &rp, &out);
    out.conflicts = td->conflicts;
    td->last = out;
    td->have_last = true;
    emit(td, &out);
    if (td->cb) td->cb(&td->last, td->cb_user);
}

/*============================================================================
 * Input
 *============================================================================*/

void wwv_time_decoder_symbol(wwv_time_decoder_t *td, float minute_start_ms, int second,
                             char symbol, float pulse_offset_ms) {
    if (!td || second < 0 || second > 59) return;
    /* No minute anchor: sync never anchors at or before stream time 0. The
     * correlator's old destroy path closed its last window with minute_start_ms
     * 0, which closed the open frame early and opened one at 0.0 (saga). */
    if (!(minute_start_ms > 0.0f)) return;
    if (td->cur_open && fabsf(minute_start_ms - td->cur.start_ms) >= SAME_FRAME_MS) close_frame(td);
    if (!td->cur_open) {
        frame_reset(&td->cur, minute_start_ms);
        td->cur_open = true;
    }
    frame_t *f = &td->cur;
    switch (symbol) {
        case '0': f->sym[second] = SYM_ZERO; break;
        case '1': f->sym[second] = SYM_ONE; break;
        case 'P': f->sym[second] = SYM_P; break;
        default:  f->sym[second] = SYM_ERASED; break;
    }
    f->boundary[second] = !isnan(pulse_offset_ms) && pulse_offset_ms <= td->cfg.boundary_ms;
    if (second == 59) close_frame(td);
}

void wwv_time_decoder_discard_frame(wwv_time_decoder_t *td) {
    if (!td || !td->cur_open) return;
    printf("[TD] Frame from %.1f ms discarded: it spans a sync phase break\n", td->cur.start_ms);
    td->cur_open = false;
}

void wwv_time_decoder_reset(wwv_time_decoder_t *td) {
    if (!td) return;
    if (td->cur_open || td->nhist > 0)
        printf("[TD] Reset: the sync phase was a whole second off; open frame and %d-frame chain dropped\n", td->nhist);
    td->cur_open = false;
    reset_chain(td);
}

void wwv_time_decoder_bcd_callback(const bcd_symbol_event_t *e, void *user_data) {
    wwv_time_decoder_t *td = (wwv_time_decoder_t *)user_data;
    /* Symbols binned under a phase that has since broken do not share a frame
     * with the ones after the break */
    if (e->phase_resets != td->cur_resets) {
        /* the phase was found wrong: what the chain holds may be in the wrong seconds */
        wwv_time_decoder_reset(td);
        td->cur_resets = e->phase_resets;
    }
    if (td->cur_open && e->phase_epoch != td->cur_epoch) wwv_time_decoder_discard_frame(td);
    wwv_time_decoder_symbol(td, e->minute_start_ms, e->second,
                            bcd_corr_symbol_char(e->symbol), e->pulse_offset_ms);
    td->cur_epoch = e->phase_epoch;
}

void wwv_time_decoder_periodic(wwv_time_decoder_t *td, float now_ms) {
    if (td && td->cur_open && now_ms >= td->cur.start_ms + FRAME_CLOSE_MS) close_frame(td);
}
