/*
 * test_lung_plot.c — Run lung model through breath cycles, output CSV.
 *
 * Build:  cc -std=c99 -Wall -o build/test_lung_plot test_lung_plot.c lung_model.c
 * Run:    ./build/test_lung_plot > build/lung_data.csv
 * Plot:   python3 plot_lung.py build/lung_data.csv
 */

#include <stdio.h>
#include "lung_model.h"

#define TICKS_PER_SEC   100          /* 10ms per tick */

/* Breath timing (ticks) */
#define T_INHALE   (1 * TICKS_PER_SEC)   /* 1s inhale  */
#define T_HOLD     (50)                   /* 0.5s hold  */
#define T_EXHALE   (2 * TICKS_PER_SEC)   /* 2s exhale  */
#define T_BREATH   (T_INHALE + T_HOLD + T_EXHALE)

#define N_BREATHS  3
#define DUTY_PCT   40   /* drive duty during inhale */

static double fp_to_double(fp16_t x)
{
    return (double)x / (1 << FP_SHIFT);
}

int main(void)
{
    lung_ctx_t lung;
    lung_init(&lung);
    lung_set_noise(&lung, 42, 5);  /* ±5% noise */

    printf("time_s,pressure_cmH2O,volume_mL,phase\n");

    int tick = 0;
    for (int b = 0; b < N_BREATHS; b++) {
        for (int t = 0; t < T_BREATH; t++, tick++) {
            int is_exhale;
            int duty;
            const char *phase;

            if (t < T_INHALE) {
                is_exhale = 0;
                duty = DUTY_PCT;
                phase = "inhale";
            } else if (t < T_INHALE + T_HOLD) {
                is_exhale = 0;
                duty = 0;
                phase = "hold";
            } else {
                is_exhale = 1;
                duty = 0;
                phase = "exhale";
            }

            lung_tick(&lung, duty, is_exhale);

            printf("%.3f,%.2f,%.2f,%s\n",
                   (double)tick / TICKS_PER_SEC,
                   fp_to_double(lung.pressure),
                   fp_to_double(lung.volume),
                   phase);
        }
    }

    return 0;
}
