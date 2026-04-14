/*
 * lung_model.c — Single-compartment lung simulation
 *
 * One implementation, two consumers:
 *   - Host tests link this directly
 *   - Renode peripheral loads it as a shared library (lung_model.so)
 *
 * Physics: V/C gives elastic pressure, R*flow gives resistive pressure.
 * During active phases the turbine drives flow proportional to duty;
 * during exhale, elastic recoil above PEEP drives passive expiration.
 * A PEEP valve clamp prevents volume from dropping below C * PEEP.
 */

#include "lung_model.h"

/* ---- Fixed-point helpers (local — not exported) ------------------------- */

static fp16_t fp_mul(fp16_t a, fp16_t b)
{
    return (fp16_t)(((int64_t)a * b) >> FP_SHIFT);
}

static fp16_t fp_div(fp16_t a, fp16_t b)
{
    if (b == 0)
        return (a >= 0) ? INT32_MAX : INT32_MIN;
    return (fp16_t)(((int64_t)a << FP_SHIFT) / b);
}

/* dt = 10ms = 0.01s in Q16.16 */
#define DT_FP       (FP_ONE / 100)
#define MAX_VOLUME  FP_FROM_INT(1000)   /* 1000 mL safety clamp */

/* ---- API ---------------------------------------------------------------- */

void lung_init(lung_ctx_t *lung)
{
    lung->compliance = FP_FROM_INT(50);
    lung->resistance = FP_FROM_INT(5);
    lung->k_turb     = FP_FROM_INT(10);
    lung->peep       = FP_FROM_INT(5);
    lung->volume     = fp_mul(lung->compliance, lung->peep);
    lung->pressure   = lung->peep;
}

fp16_t lung_tick(lung_ctx_t *lung, uint32_t duty_pct, int is_exhale)
{
    fp16_t flow;

    if (is_exhale) {
        /* Passive exhale: flow driven by pressure above PEEP */
        fp16_t p_elastic = fp_div(lung->volume, lung->compliance);
        fp16_t p_drive = p_elastic - lung->peep;
        if (p_drive < 0)
            p_drive = 0;
        flow = -fp_div(p_drive, lung->resistance) * 1000;
    } else {
        /* Active: turbine drives flow = k_turb * duty */
        flow = fp_mul(lung->k_turb, FP_FROM_INT((int32_t)duty_pct));
    }

    lung->volume += fp_mul(flow, DT_FP);

    /* PEEP valve: clamp volume so pressure >= PEEP */
    fp16_t min_vol = fp_mul(lung->compliance, lung->peep);
    if (lung->volume < min_vol)
        lung->volume = min_vol;

    /* Safety clamp */
    if (lung->volume > MAX_VOLUME)
        lung->volume = MAX_VOLUME;

    /* Airway pressure: P = V/C + R*flow/1000 */
    fp16_t p_elastic   = fp_div(lung->volume, lung->compliance);
    fp16_t p_resistive = fp_mul(lung->resistance, flow) / 1000;
    lung->pressure = p_elastic + p_resistive;
    if (lung->pressure < 0)
        lung->pressure = 0;

    return lung->pressure;
}
