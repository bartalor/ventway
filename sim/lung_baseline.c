/*
 * lung_baseline.c — Run lung model with no ventilator (p_source = 0).
 *
 * Prints pressure stats at the same time points as a ventilated run
 * (every breath cycle boundary) so the two can be compared.
 *
 * Build:  make build/lung_baseline
 * Run:    ./build/lung_baseline
 */

#include <stdio.h>
#include "lung_model.h"

/* Match firmware timing: 10ms ticks */
#define TICKS_PER_SEC   100

/* Match firmware breath cycle: INHALE=1.0s, HOLD=0.5s, EXHALE=2.0s = 3.5s */
#define INHALE_TICKS    100
#define HOLD_TICKS       50
#define EXHALE_TICKS    200
#define CYCLE_TICKS     (INHALE_TICKS + HOLD_TICKS + EXHALE_TICKS)

#define N_CYCLES        8
#define T_TOTAL         (N_CYCLES * CYCLE_TICKS)

static double fp_to_double(fp16_t x)
{
    return (double)x / (1 << FP_SHIFT);
}

/* Per-state accumulators */
typedef struct {
    const char *name;
    double target;
    double sum_err;
    double sum_abs_err;
    double sum_sq_err;
    double min_p;
    double max_p;
    int n;
} state_stats_t;

static void stats_add(state_stats_t *s, double pressure)
{
    double err = pressure - s->target;
    s->sum_err     += err;
    s->sum_abs_err += (err < 0 ? -err : err);
    s->sum_sq_err  += err * err;
    if (pressure < s->min_p) s->min_p = pressure;
    if (pressure > s->max_p) s->max_p = pressure;
    s->n++;
}

static void stats_print(const state_stats_t *s)
{
    if (s->n == 0) return;
    double mean_err     = s->sum_err     / s->n;
    double mean_abs_err = s->sum_abs_err / s->n;
    double rms_err;
    {
        double v = s->sum_sq_err / s->n;
        /* integer sqrt approx — good enough for display */
        rms_err = v;
        for (int i = 0; i < 20; i++)
            rms_err = (rms_err + v / rms_err) / 2.0;
    }

    printf("--- %s (target: %.0f cmH2O, n=%d) ---\n", s->name, s->target, s->n);
    printf("  Pressure range:  %.1f - %.1f cmH2O\n", s->min_p, s->max_p);
    printf("  Mean error:      %+.2f cmH2O\n", mean_err);
    printf("  Mean |error|:    %.2f cmH2O\n", mean_abs_err);
    printf("  Max  |error|:    %.2f cmH2O\n",
           (s->max_p - s->target > s->target - s->min_p)
           ? s->max_p - s->target : s->target - s->min_p);
    printf("  RMS error:       %.2f cmH2O\n", rms_err);
    printf("\n");
}

int main(void)
{
    lung_ctx_t lung;
    lung_init(&lung);

    /* Targets match firmware defaults */
    state_stats_t inhale = { "INHALE", 20.0, 0,0,0, 1e9, -1e9, 0 };
    state_stats_t hold   = { "HOLD",   20.0, 0,0,0, 1e9, -1e9, 0 };
    state_stats_t exhale = { "EXHALE",  5.0, 0,0,0, 1e9, -1e9, 0 };

    for (int t = 0; t < T_TOTAL; t++) {
        lung_tick(&lung, 0);

        int phase_tick = t % CYCLE_TICKS;
        double p = fp_to_double(lung.pressure);

        if (phase_tick < INHALE_TICKS)
            stats_add(&inhale, p);
        else if (phase_tick < INHALE_TICKS + HOLD_TICKS)
            stats_add(&hold, p);
        else
            stats_add(&exhale, p);
    }

    printf("Lung baseline: %d cycles, p_source = 0 (no ventilator)\n\n", N_CYCLES);
    stats_print(&inhale);
    stats_print(&hold);
    stats_print(&exhale);

    return 0;
}
