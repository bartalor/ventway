/*
 * lung_model.h — Single-compartment lung simulation (pure RC model)
 *
 * This is the "patient" — it lives outside the ventilator firmware.
 * Used by:
 *   - Renode peripheral (lung_model.py loads lung_model.so via ctypes)
 *   - Host tests (linked directly into test binary)
 *
 * The lung knows nothing about ventilators, duty cycles, or PEEP.
 * It receives a source pressure and responds with airway pressure.
 * The firmware never includes this header.
 */

#ifndef LUNG_MODEL_H
#define LUNG_MODEL_H

#include <stdint.h>

/* Re-use the same Q16.16 definitions as the firmware */
typedef int32_t fp16_t;

#define FP_SHIFT        16
#define FP_ONE          (1 << FP_SHIFT)
#define FP_FROM_INT(x)  ((fp16_t)((int32_t)(x) * FP_ONE))

/* ---- Lung context ------------------------------------------------------- */

typedef struct {
    fp16_t volume;       /* lung volume, mL (Q16.16) */
    fp16_t compliance;   /* mL/cmH2O (Q16.16) */
    fp16_t resistance;   /* cmH2O/(L/s) (Q16.16) */
    fp16_t pressure;     /* last computed airway pressure (Q16.16) */
    uint32_t noise_seed; /* PRNG state (0 = noise disabled) */
    fp16_t noise_pct;    /* max perturbation as % of nominal (Q16.16) */
} lung_ctx_t;

/* ---- API ---------------------------------------------------------------- */

/*
 * Initialize lung with physiological defaults.
 *   compliance = 50 mL/cmH2O
 *   resistance = 5 cmH2O/(L/s)
 *   volume     = C * PEEP_DEFAULT (functional residual capacity)
 *
 * PEEP_DEFAULT (5 cmH2O) is used only to set the initial volume.
 * The lung model itself has no concept of PEEP.
 */
void lung_init(lung_ctx_t *lung);

/*
 * Advance the lung model by one 10ms tick.
 *   p_source:  applied pressure at the airway opening, cmH2O (Q16.16).
 *              The caller decides what this is — the lung just responds.
 *
 * Returns airway pressure in Q16.16 cmH2O.
 * Also stored in lung->pressure.
 */
fp16_t lung_tick(lung_ctx_t *lung, fp16_t p_source);

/*
 * Enable breath-to-breath noise on compliance and resistance.
 *   seed:    nonzero PRNG seed (0 disables noise)
 *   pct:     max perturbation as integer percentage (e.g. 5 = ±5%)
 */
void lung_set_noise(lung_ctx_t *lung, uint32_t seed, int pct);

#endif /* LUNG_MODEL_H */
