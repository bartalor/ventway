/*
 * test_lung_plot.c — Lung model: open-loop vs closed-loop comparison.
 *
 * Outputs a single CSV with two modes:
 *   "open"   — fixed pressure square wave drives the lung directly
 *   "closed" — Ventway PID controller drives the lung via feedback
 *
 * Build:  cc -std=c99 -Wall -o build/test_lung_plot test_lung_plot.c lung_model.c ventway.c
 * Run:    ./build/test_lung_plot > build/lung_data.csv
 * Plot:   python3 plot_lung.py build/lung_data.csv
 */

#include <stdio.h>
#include "lung_model.h"
#include "ventway.h"

#define TICKS_PER_SEC   100          /* 10ms per tick */

/* Breath timing (ticks) */
#define T_INHALE   (1 * TICKS_PER_SEC)   /* 1s inhale  */
#define T_HOLD     (50)                   /* 0.5s hold  */
#define T_EXHALE   (2 * TICKS_PER_SEC)   /* 2s exhale  */
#define T_BREATH   (T_INHALE + T_HOLD + T_EXHALE)

#define N_BREATHS  3

/* System parameters (same as test harness and Renode glue) */
#define K_DRIVE     (FP_ONE / 2)    /* 0.5 cmH2O per %duty */
#define PEEP_CMHO   FP_FROM_INT(5)

/* Pressure targets (match firmware defaults) */
#define P_INHALE    FP_FROM_INT(20)
#define P_EXHALE    PEEP_CMHO

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
        p_source = p_lung;  /* sealed airway */
    return p_source;
}

/*
 * Open-loop: apply a fixed pressure square wave.
 * No feedback, no PID — just raw lung response to ideal drive.
 */
static void run_open_loop(void)
{
    lung_ctx_t lung;
    lung_init(&lung);
    lung_set_noise(&lung, 42, 5);

    int tick = 0;
    for (int b = 0; b < N_BREATHS; b++) {
        for (int t = 0; t < T_BREATH; t++, tick++) {
            fp16_t p_source;
            fp16_t target;
            const char *phase;

            if (t < T_INHALE) {
                p_source = P_INHALE;
                target = P_INHALE;
                phase = "inhale";
            } else if (t < T_INHALE + T_HOLD) {
                p_source = P_INHALE;  /* hold at inspiratory pressure */
                target = P_INHALE;
                phase = "hold";
            } else {
                p_source = P_EXHALE;
                target = P_EXHALE;
                phase = "exhale";
            }

            lung_tick(&lung, p_source);

            printf("%.3f,%.2f,%.2f,%.2f,%s,open\n",
                   (double)tick / TICKS_PER_SEC,
                   fp_to_double(lung.pressure),
                   fp_to_double(lung.volume),
                   fp_to_double(target),
                   phase);
        }
    }
}

/*
 * Closed-loop: Ventway PID controller drives the lung.
 * The controller reads pressure, computes duty, which maps to p_source.
 */
static void run_closed_loop(void)
{
    ventway_ctx_t ctx;
    lung_ctx_t lung;
    ventway_init(&ctx);
    lung_init(&lung);
    lung_set_noise(&lung, 42, 5);  /* same seed — same lung */

    /* Wire sensor to lung pressure */
    fp16_t sensor_val = lung.pressure;
    ctx.sensor_reg = &sensor_val;

    enter_state(&ctx, INHALE);

    int tick = 0;
    for (int b = 0; b < N_BREATHS; b++) {
        for (int t = 0; t < T_BREATH; t++, tick++) {
            int is_exhale;
            const char *phase;
            fp16_t target;

            if (t < T_INHALE) {
                if (t == 0 && b > 0) enter_state(&ctx, INHALE);
                is_exhale = 0;
                target = P_INHALE;
                phase = "inhale";
            } else if (t < T_INHALE + T_HOLD) {
                if (t == T_INHALE) enter_state(&ctx, HOLD);
                is_exhale = 0;
                target = P_INHALE;
                phase = "hold";
            } else {
                if (t == T_INHALE + T_HOLD) enter_state(&ctx, EXHALE);
                is_exhale = 1;
                target = P_EXHALE;
                phase = "exhale";
            }

            /* PID step */
            sensor_read(&ctx);
            pid_tick(&ctx);

            /* Drive the lung */
            fp16_t p_src = drive_p_source(ctx.duty_pct, is_exhale, lung.pressure);
            sensor_val = lung_tick(&lung, p_src);

            printf("%.3f,%.2f,%.2f,%.2f,%s,closed\n",
                   (double)tick / TICKS_PER_SEC,
                   fp_to_double(lung.pressure),
                   fp_to_double(lung.volume),
                   fp_to_double(target),
                   phase);
        }
    }
}

int main(void)
{
    printf("time_s,pressure_cmH2O,volume_mL,target_cmH2O,phase,mode\n");
    run_open_loop();
    run_closed_loop();
    return 0;
}
