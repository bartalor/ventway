/*
 * test_lung_plot.c — Lung alone vs lung + ventilator.
 *
 * Build:  cc -std=c99 -Wall -o build/test_lung_plot test_lung_plot.c lung_model.c ventway.c
 * Run:    ./build/test_lung_plot > build/lung_data.csv
 * Plot:   python3 plot_lung.py build/lung_data.csv
 */

#include <stdio.h>
#include "lung_model.h"
#include "ventway.h"

#define TICKS_PER_SEC   100
#define T_TOTAL         (3 * 350)   /* 3 breath cycles at 3.5s each */

/* System parameters (ventilator side) */
#define K_DRIVE     (FP_ONE / 2)
#define PEEP_CMHO   FP_FROM_INT(5)

static double fp_to_double(fp16_t x)
{
    return (double)x / (1 << FP_SHIFT);
}

static fp16_t drive_p_source(uint32_t duty_pct, int is_exhale, fp16_t p_lung)
{
    if (is_exhale)
        return PEEP_CMHO;
    fp16_t p_source = fp_mul(K_DRIVE, FP_FROM_INT((int32_t)duty_pct));
    if (p_source < p_lung)
        p_source = p_lung;
    return p_source;
}

int main(void)
{
    lung_ctx_t lung_alone, lung_vent;
    ventway_ctx_t ctx;
    fp16_t sensor_val;

    lung_init(&lung_alone);
    lung_init(&lung_vent);
    lung_set_noise(&lung_alone, 42, 5);
    lung_set_noise(&lung_vent, 42, 5);

    ventway_init(&ctx);
    sensor_val = lung_vent.pressure;
    ctx.sensor_reg = &sensor_val;
    enter_state(&ctx, INHALE);

    printf("time_s,alone_pressure,alone_volume,vent_pressure,vent_volume,target\n");

    for (int t = 0; t < T_TOTAL; t++) {
        /* Lung alone: no ventilator, p_source = 0 */
        lung_tick(&lung_alone, 0);

        /* Lung + vent: PID drives the lung */
        sensor_read(&ctx);
        pid_tick(&ctx);
        state_machine_tick(&ctx);
        int is_exhale = (ctx.state == EXHALE);
        sensor_val = lung_tick(&lung_vent, drive_p_source(ctx.duty_pct, is_exhale, lung_vent.pressure));

        printf("%.3f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
               (double)t / TICKS_PER_SEC,
               fp_to_double(lung_alone.pressure),
               fp_to_double(lung_alone.volume),
               fp_to_double(lung_vent.pressure),
               fp_to_double(lung_vent.volume),
               fp_to_double(ctx.pressure_target[ctx.state]));
    }

    return 0;
}
