/*
 * ventway.h — Ventilator state machine and TX buffer declarations
 *
 * All mutable state lives in ventway_ctx_t. Functions take an explicit
 * context pointer — no hidden globals.
 */

#ifndef VENTWAY_H
#define VENTWAY_H

#include <stdint.h>

/* ---- Constants ---------------------------------------------------------- */

#define SYS_CLK         16000000U
#define TICK_MS         10U
#define TX_BUF_SIZE     256U  /* must be power of 2 */
#define RX_BUF_SIZE     64U   /* must be power of 2 */
#define CMD_BUF_SIZE    64U
#define DUTY_MAX_PCT    FP_FROM_INT(100)
#define PID_ALPHA       ((fp16_t)((998L * FP_ONE) / 1000))  /* 0.998 leaky integrator */

/* ---- Fixed-point Q16.16 ------------------------------------------------- */

typedef int32_t fp16_t;

#define FP_SHIFT        16
#define FP_ONE          (1 << FP_SHIFT)
#define FP_FROM_INT(x)  ((fp16_t)((int32_t)(x) * FP_ONE))
#define FP_TO_INT(x)    ((x) >> FP_SHIFT)

static inline fp16_t fp_mul(fp16_t a, fp16_t b)
{
    return (fp16_t)(((int64_t)a * b) >> FP_SHIFT);
}

static inline fp16_t fp_div(fp16_t a, fp16_t b)
{
    if (b == 0)
        return (a >= 0) ? INT32_MAX : INT32_MIN;  /* saturate on div-by-zero */
    return (fp16_t)(((int64_t)a << FP_SHIFT) / b);
}

/* dt = 10ms = 0.01s in Q16.16 */
#define DT_FP           (FP_ONE / 100)

/* ---- State machine types ------------------------------------------------ */

typedef enum {
    INHALE,
    HOLD,
    EXHALE,
    STATE_COUNT  /* must be last — used for bounds checking */
} state_t;

extern const char *const state_names[STATE_COUNT];
extern const uint32_t    state_duration_ms[STATE_COUNT];
extern const fp16_t      default_pressure_target[STATE_COUNT];

/* ---- Context struct ----------------------------------------------------- */

typedef struct {
    /* TX ring buffer */
    char     tx_buf[TX_BUF_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_overflow;       /* count of dropped characters */

    /* RX ring buffer (ISR writes, main loop reads) */
    char     rx_buf[RX_BUF_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    /* Command line buffer */
    char     cmd_buf[CMD_BUF_SIZE];
    uint32_t cmd_len;

    /* Per-instance timing configuration (copied from defaults at init) */
    uint32_t duration_ms[STATE_COUNT];

    /* Per-state pressure targets in cmH2O (Q16.16) */
    fp16_t   pressure_target[STATE_COUNT];

    /* Pressure sensor — sensor_read() reads from this register each tick.
     * On real hardware: points to ADC data register.
     * In tests: points to a local variable for injection. */
    volatile fp16_t *sensor_reg;
    fp16_t   pressure;          /* last sensor reading, cmH2O (Q16.16) */

    /* PID controller state */
    fp16_t   pid_integral;
    fp16_t   pid_prev_meas;     /* previous pressure measurement (derivative-on-measurement) */

    /* PID gains (tunable) */
    fp16_t   kp;
    fp16_t   ki;
    fp16_t   kd;
    fp16_t   kb;                /* anti-windup back-calculation gain */

    /* State machine */
    state_t  state;
    uint32_t cycle_count;
    uint32_t tick_count;
    uint32_t state_ticks;
    uint32_t duty_pct;
    uint32_t state_changed;     /* flag: ISR sets, main loop clears after logging */
} ventway_ctx_t;

/* ---- TX buffer API ------------------------------------------------------ */

void     ventway_init(ventway_ctx_t *ctx);
void     tx_put(ventway_ctx_t *ctx, char c);
void     tx_puts(ventway_ctx_t *ctx, const char *s);
void     tx_put_uint(ventway_ctx_t *ctx, uint32_t n);
void     tx_put_fp(ventway_ctx_t *ctx, fp16_t val, int decimals);
uint32_t tx_read(ventway_ctx_t *ctx, char *dst, uint32_t max_len);

/* ---- RX buffer API ------------------------------------------------------ */

void rx_put(ventway_ctx_t *ctx, char c);
int  rx_get(ventway_ctx_t *ctx, char *out);

/* ---- Command processing API --------------------------------------------- */

void cmd_process_byte(ventway_ctx_t *ctx, char c);
void cmd_execute(ventway_ctx_t *ctx);

/* ---- Pressure sensor API ------------------------------------------------ */

/*
 * Read airway pressure from hardware sensor register.
 * On real hardware this reads an ADC. In simulation, a Renode peripheral
 * (lung model) writes the pressure value to this register.
 *
 * sensor_read() is called once per tick before pid_tick().
 */
void sensor_read(ventway_ctx_t *ctx);

/* ---- PID controller API ------------------------------------------------- */

void pid_tick(ventway_ctx_t *ctx);

/* ---- State machine API -------------------------------------------------- */

void enter_state(ventway_ctx_t *ctx, state_t s);
void state_log(ventway_ctx_t *ctx);
int  state_machine_tick(ventway_ctx_t *ctx);

#endif /* VENTWAY_H */
