/*
 * lung_model.h — Single-compartment lung simulation
 *
 * This is the "patient" — it lives outside the ventilator firmware.
 * Used by:
 *   - Renode peripheral (lung_model.py loads lung_model.so via ctypes)
 *   - Host tests (linked directly into test binary)
 *
 * The firmware never includes this header. The controller and the
 * patient communicate only through hardware registers.
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
    fp16_t k_drive;      /* source pressure gain: cmH2O per %duty (Q16.16) */
    fp16_t peep;         /* PEEP pressure target, cmH2O (Q16.16) */
    fp16_t pressure;     /* last computed airway pressure (Q16.16) */
    uint32_t noise_seed; /* PRNG state (0 = noise disabled) */
    fp16_t noise_pct;    /* max perturbation as % of nominal (Q16.16, e.g. 5% = FP_FROM_INT(5)) */
} lung_ctx_t;

/* ---- API ---------------------------------------------------------------- */

/*
 * Initialize lung with physiological defaults.
 *   compliance = 50 mL/cmH2O
 *   resistance = 5 cmH2O/(L/s)
 *   k_drive    = 0.5 cmH2O per %duty (50 cmH2O at full drive)
 *   PEEP       = 5 cmH2O
 *   volume     = C * PEEP (functional residual capacity)
 */
void lung_init(lung_ctx_t *lung);

/*
 * Advance the lung model by one 10ms tick.
 *   duty_pct:   drive duty cycle 0–100 (from PWM output)
 *   is_exhale:  nonzero if passive exhale phase (duty should be 0)
 *
 * Returns airway pressure in Q16.16 cmH2O.
 * Also stored in lung->pressure.
 */
fp16_t lung_tick(lung_ctx_t *lung, uint32_t duty_pct, int is_exhale);

/*
 * Enable breath-to-breath noise on compliance and resistance.
 *   seed:    nonzero PRNG seed (0 disables noise)
 *   pct:     max perturbation as integer percentage (e.g. 5 = ±5%)
 */
void lung_set_noise(lung_ctx_t *lung, uint32_t seed, int pct);

#endif /* LUNG_MODEL_H */
