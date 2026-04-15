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

/* ---- Simple LCG PRNG (no stdlib dependency) ----------------------------- */

static uint32_t prng_next(uint32_t *state)
{
    *state = *state * 1103515245U + 12345U;
    return (*state >> 16) & 0x7FFF;  /* 0..32767 */
}

/*
 * Return a random perturbation in [-pct, +pct] percent of nominal.
 * Result is a multiplier: FP_ONE ± (pct/100) in Q16.16.
 */
static fp16_t noise_multiplier(uint32_t *seed, fp16_t pct)
{
    if (*seed == 0 || pct == 0)
        return FP_ONE;
    uint32_t r = prng_next(seed);
    /* Map 0..32767 → -pct..+pct in Q16.16 */
    fp16_t offset = (fp16_t)((int64_t)pct * (2 * (int32_t)r - 32767) / 32767);
    /* Convert from percentage to fraction: offset/100 */
    return FP_ONE + fp_div(offset, FP_FROM_INT(100));
}

/* dt = 10ms = 0.01s in Q16.16.  Exact: 65536/100 = 655.36; round to nearest. */
#define DT_FP       ((FP_ONE + 50) / 100)       /* 656 ≈ 0.01001 */
#define MAX_VOLUME  FP_FROM_INT(1000)            /* 1000 mL safety clamp */
#define ML_PER_L    1000                         /* mL/s ↔ L/s conversion */

/* ---- API ---------------------------------------------------------------- */

void lung_init(lung_ctx_t *const lung)
{
    lung->compliance = FP_FROM_INT(50);
    lung->resistance = FP_FROM_INT(5);
    lung->k_turb     = FP_FROM_INT(10);
    lung->peep       = FP_FROM_INT(5);
    lung->volume     = fp_mul(lung->compliance, lung->peep);
    lung->pressure   = lung->peep;
    lung->noise_seed = 0;
    lung->noise_pct  = 0;
}

void lung_set_noise(lung_ctx_t *const lung, uint32_t seed, int pct)
{
    if (pct < 0) pct = 0;  /* reject negative — clamp to disabled */
    lung->noise_seed = seed;
    lung->noise_pct  = FP_FROM_INT(pct);
}

fp16_t lung_tick(lung_ctx_t *const lung, uint32_t duty_pct, int is_exhale)
{
    /* Apply per-tick noise to compliance and resistance */
    fp16_t C = fp_mul(lung->compliance, noise_multiplier(&lung->noise_seed, lung->noise_pct));
    fp16_t R = fp_mul(lung->resistance, noise_multiplier(&lung->noise_seed, lung->noise_pct));
    if (C < FP_ONE) C = FP_ONE;   /* clamp to >= 1 mL/cmH2O */
    if (R < FP_ONE) R = FP_ONE;   /* clamp to >= 1 cmH2O/(L/s) */

    fp16_t flow;

    if (is_exhale) {
        /* Passive exhale: flow driven by pressure above PEEP */
        fp16_t p_elastic = fp_div(lung->volume, C);
        fp16_t p_drive = p_elastic - lung->peep;
        if (p_drive < 0)
            p_drive = 0;
        flow = -fp_div(p_drive, R) * ML_PER_L;  /* L/s → mL/s */
    } else {
        /* Active: turbine drives flow = k_turb * duty */
        flow = fp_mul(lung->k_turb, FP_FROM_INT((int32_t)duty_pct));
    }

    lung->volume += fp_mul(flow, DT_FP);

    /* PEEP valve: clamp volume so pressure >= PEEP */
    fp16_t min_vol = fp_mul(C, lung->peep);
    if (lung->volume < min_vol)
        lung->volume = min_vol;

    /* Safety clamp */
    if (lung->volume > MAX_VOLUME)
        lung->volume = MAX_VOLUME;

    /* Airway pressure: P = V/C + R*flow/ML_PER_L */
    fp16_t p_elastic   = fp_div(lung->volume, C);
    fp16_t p_resistive = fp_mul(R, flow) / ML_PER_L;  /* mL/s → L/s */
    lung->pressure = p_elastic + p_resistive;
    if (lung->pressure < 0)
        lung->pressure = 0;

    return lung->pressure;
}
