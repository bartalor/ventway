/*
 * ventway.c — Ventilator state machine and TX buffer implementation
 */

#include "ventway.h"

/* ---- Compile-time invariants -------------------------------------------- */

_Static_assert((TX_BUF_SIZE & (TX_BUF_SIZE - 1)) == 0,
               "TX_BUF_SIZE must be a power of 2");

_Static_assert((RX_BUF_SIZE & (RX_BUF_SIZE - 1)) == 0,
               "RX_BUF_SIZE must be a power of 2");

_Static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_COUNT,
               "state_names must have exactly STATE_COUNT entries");

_Static_assert(sizeof(state_duration_ms) / sizeof(state_duration_ms[0]) == STATE_COUNT,
               "state_duration_ms must have exactly STATE_COUNT entries");

_Static_assert(sizeof(default_pressure_target) / sizeof(default_pressure_target[0]) == STATE_COUNT,
               "default_pressure_target must have exactly STATE_COUNT entries");

/* ---- Lookup tables ------------------------------------------------------ */

const char *const state_names[STATE_COUNT] = {
    [INHALE] = "INHALE",
    [HOLD]   = "HOLD",
    [EXHALE] = "EXHALE",
};

const uint32_t state_duration_ms[STATE_COUNT] = {
    [INHALE] = 1000,
    [HOLD]   =  500,
    [EXHALE] = 1500,
};

/*
 * Pressure targets in cmH2O (Q16.16 fixed-point).
 *
 * Physiological model — Pressure Control Ventilation (PCV):
 *   INHALE  20 cmH2O — inspiratory pressure, drives air into lungs
 *   HOLD    20 cmH2O — plateau pressure for gas exchange
 *   EXHALE   5 cmH2O — PEEP, prevents alveolar collapse
 *
 * The PID controller chases these targets. The decelerating inspiratory
 * flow pattern emerges naturally: at INHALE start the lungs are empty
 * (large pressure gap → high duty), as they fill the gap shrinks and
 * the controller backs off (low duty). No hand-tuned duty curves needed.
 */
const fp16_t default_pressure_target[STATE_COUNT] = {
    [INHALE] = FP_FROM_INT(20),
    [HOLD]   = FP_FROM_INT(20),
    [EXHALE] = FP_FROM_INT(5),
};

/* ---- TX ring buffer ----------------------------------------------------- */

void ventway_init(ventway_ctx_t *ctx)
{
    ctx->tx_head     = 0;
    ctx->tx_tail     = 0;
    ctx->tx_overflow = 0;
    ctx->rx_head    = 0;
    ctx->rx_tail    = 0;
    ctx->cmd_len    = 0;
    ctx->state      = INHALE;
    ctx->cycle_count = 0;
    ctx->tick_count  = 0;
    ctx->state_ticks   = 0;
    ctx->duty_pct      = 0;
    ctx->state_changed = 0;

    /* Timing defaults */
    for (int i = 0; i < STATE_COUNT; i++) {
        ctx->duration_ms[i]      = state_duration_ms[i];
        ctx->pressure_target[i]  = default_pressure_target[i];
    }

    /* Pressure sensor */
    ctx->sensor_reg = (volatile fp16_t *)0;  /* must be set before use */
    ctx->pressure   = 0;

    /* PID controller */
    ctx->pid_integral  = 0;
    ctx->pid_prev_meas = 0;
    ctx->kp = FP_FROM_INT(3);             /* 3.0 */
    ctx->ki = FP_ONE;                     /* 1.0 */
    ctx->kd = FP_ONE / 10;               /* 0.1 */
    ctx->kb = FP_FROM_INT(2);             /* 2.0 */
}

void tx_put(ventway_ctx_t *ctx, char c)
{
    uint32_t next = (ctx->tx_head + 1) & (TX_BUF_SIZE - 1);
    if (next == ctx->tx_tail) {
        ctx->tx_overflow++;
        return;
    }
    ctx->tx_buf[ctx->tx_head] = c;
    ctx->tx_head = next;
}

void tx_puts(ventway_ctx_t *ctx, const char *s)
{
    while (*s)
        tx_put(ctx, *s++);
}

void tx_put_uint(ventway_ctx_t *ctx, uint32_t n)
{
    char buf[11];
    int i = 0;

    if (n == 0) {
        tx_put(ctx, '0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (char)(n % 10);
        n /= 10;
    }
    while (i--)
        tx_put(ctx, buf[i]);
}

void tx_put_fp(ventway_ctx_t *ctx, fp16_t val, int decimals)
{
    if (val < 0) {
        tx_put(ctx, '-');
        val = -val;
    }
    tx_put_uint(ctx, (uint32_t)(val >> FP_SHIFT));
    if (decimals > 0) {
        tx_put(ctx, '.');
        uint32_t frac = (uint32_t)(val & (FP_ONE - 1));
        for (int d = 0; d < decimals; d++) {
            frac *= 10;
            tx_put(ctx, '0' + (char)(frac >> FP_SHIFT));
            frac &= (FP_ONE - 1);
        }
    }
}

uint32_t tx_read(ventway_ctx_t *ctx, char *dst, uint32_t max_len)
{
    uint32_t n = 0;
    while (ctx->tx_tail != ctx->tx_head && n < max_len - 1) {
        dst[n++] = ctx->tx_buf[ctx->tx_tail];
        ctx->tx_tail = (ctx->tx_tail + 1) & (TX_BUF_SIZE - 1);
    }
    dst[n] = '\0';
    return n;
}

/* ---- RX ring buffer ----------------------------------------------------- */

void rx_put(ventway_ctx_t *ctx, char c)
{
    uint32_t next = (ctx->rx_head + 1) & (RX_BUF_SIZE - 1);
    if (next == ctx->rx_tail)
        return;  /* buffer full — drop character */
    ctx->rx_buf[ctx->rx_head] = c;
    ctx->rx_head = next;
}

int rx_get(ventway_ctx_t *ctx, char *out)
{
    if (ctx->rx_tail == ctx->rx_head)
        return 0;
    *out = ctx->rx_buf[ctx->rx_tail];
    ctx->rx_tail = (ctx->rx_tail + 1) & (RX_BUF_SIZE - 1);
    return 1;
}

/* ---- Command processing ------------------------------------------------- */

static int str_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static uint32_t parse_uint(const char *s, int *ok)
{
    uint32_t n = 0;
    *ok = 0;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') { *ok = 0; return 0; }
        uint32_t digit = (uint32_t)(*s - '0');
        if (n > (UINT32_MAX - digit) / 10) { *ok = 0; return 0; }  /* overflow */
        n = n * 10 + digit;
        s++;
    }
    *ok = 1;
    return n;
}

/* Find next token in buf starting at *pos, write pointer into *tok, return length.
   Tokens are separated by spaces. Mutates buf (inserts NULs). */
static int next_token(char *buf, uint32_t len, uint32_t *pos, char **tok)
{
    while (*pos < len && buf[*pos] == ' ')
        (*pos)++;
    if (*pos >= len) return 0;
    *tok = &buf[*pos];
    while (*pos < len && buf[*pos] != ' ')
        (*pos)++;
    if (*pos < len)
        buf[(*pos)++] = '\0';
    return 1;
}

static state_t parse_state_name(const char *name)
{
    if (str_eq(name, "inhale")) return INHALE;
    if (str_eq(name, "hold"))   return HOLD;
    if (str_eq(name, "exhale")) return EXHALE;
    return STATE_COUNT;  /* invalid */
}

void cmd_execute(ventway_ctx_t *ctx)
{
    ctx->cmd_buf[ctx->cmd_len] = '\0';
    uint32_t pos = 0;
    char *verb;

    if (!next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &verb))
        return;

    if (str_eq(verb, "status")) {
        for (int i = 0; i < STATE_COUNT; i++) {
            tx_puts(ctx, state_names[i]);
            tx_puts(ctx, ": ");
            tx_put_uint(ctx, ctx->duration_ms[i]);
            tx_puts(ctx, "ms target=");
            tx_put_fp(ctx, ctx->pressure_target[i], 0);
            tx_puts(ctx, "cmH2O\r\n");
        }
        tx_puts(ctx, "Kp=");
        tx_put_fp(ctx, ctx->kp, 1);
        tx_puts(ctx, " Ki=");
        tx_put_fp(ctx, ctx->ki, 1);
        tx_puts(ctx, " Kd=");
        tx_put_fp(ctx, ctx->kd, 1);
        tx_puts(ctx, " Kb=");
        tx_put_fp(ctx, ctx->kb, 1);
        tx_puts(ctx, "\r\nP=");
        tx_put_fp(ctx, ctx->pressure, 1);
        tx_puts(ctx, "cmH2O duty=");
        tx_put_uint(ctx, ctx->duty_pct);
        tx_puts(ctx, "%");
        if (ctx->tx_overflow > 0) {
            tx_puts(ctx, " TX_DROP=");
            tx_put_uint(ctx, ctx->tx_overflow);
        }
        tx_puts(ctx, "\r\n");
        return;
    }

    /* "inhale 2000" / "hold 300" / "exhale 1500" — set duration */
    state_t st = parse_state_name(verb);
    if (st != STATE_COUNT) {
        char *arg;
        if (!next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &arg)) {
            tx_puts(ctx, "usage: ");
            tx_puts(ctx, state_names[st]);
            tx_puts(ctx, " <ms>\r\n");
            return;
        }
        int ok;
        uint32_t val = parse_uint(arg, &ok);
        if (!ok || val == 0) {
            tx_puts(ctx, "bad value\r\n");
            return;
        }
        if (val % TICK_MS != 0) {
            tx_puts(ctx, "must be multiple of ");
            tx_put_uint(ctx, TICK_MS);
            tx_puts(ctx, "ms\r\n");
            return;
        }
        ctx->duration_ms[st] = val;
        tx_puts(ctx, state_names[st]);
        tx_puts(ctx, " = ");
        tx_put_uint(ctx, val);
        tx_puts(ctx, "ms\r\n");
        return;
    }

    /* "target inhale 25" — set pressure target */
    if (str_eq(verb, "target")) {
        char *state_arg, *val_arg;
        if (!next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &state_arg) ||
            !next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &val_arg)) {
            tx_puts(ctx, "usage: target <state> <cmH2O>\r\n");
            return;
        }
        state_t dst = parse_state_name(state_arg);
        if (dst == STATE_COUNT) {
            tx_puts(ctx, "bad state\r\n");
            return;
        }
        int ok;
        uint32_t val = parse_uint(val_arg, &ok);
        if (!ok || val > 50) {
            tx_puts(ctx, "bad value (0-50)\r\n");
            return;
        }
        ctx->pressure_target[dst] = FP_FROM_INT((int32_t)val);
        tx_puts(ctx, state_names[dst]);
        tx_puts(ctx, " target = ");
        tx_put_uint(ctx, val);
        tx_puts(ctx, " cmH2O\r\n");
        return;
    }

    /* PID gain commands: "kp 30" means Kp=3.0 (tenths), same for ki/kd/kb */
    if (str_eq(verb, "kp") || str_eq(verb, "ki") ||
        str_eq(verb, "kd") || str_eq(verb, "kb")) {
        char *arg;
        if (!next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &arg)) {
            tx_puts(ctx, "usage: ");
            tx_puts(ctx, verb);
            tx_puts(ctx, " <tenths>\r\n");
            return;
        }
        int ok;
        uint32_t val = parse_uint(arg, &ok);
        if (!ok || val > 1000) {
            tx_puts(ctx, "bad value (0-1000)\r\n");
            return;
        }
        fp16_t fp_val = (fp16_t)((int64_t)val * FP_ONE / 10);
        if (str_eq(verb, "kp")) ctx->kp = fp_val;
        else if (str_eq(verb, "ki")) ctx->ki = fp_val;
        else if (str_eq(verb, "kd")) ctx->kd = fp_val;
        else ctx->kb = fp_val;
        tx_puts(ctx, verb);
        tx_puts(ctx, " = ");
        tx_put_fp(ctx, fp_val, 1);
        tx_puts(ctx, "\r\n");
        return;
    }

    tx_puts(ctx, "unknown cmd\r\n");
}

void cmd_process_byte(ventway_ctx_t *ctx, char c)
{
    if (c == '\r' || c == '\n') {
        if (ctx->cmd_len > 0) {
            cmd_execute(ctx);
            ctx->cmd_len = 0;
        }
        return;
    }
    if (ctx->cmd_len < CMD_BUF_SIZE - 1)
        ctx->cmd_buf[ctx->cmd_len++] = c;
}

/* ---- Pressure sensor ---------------------------------------------------- */

void sensor_read(ventway_ctx_t *ctx)
{
    if (!ctx->sensor_reg)
        for (;;);  /* trap: sensor_reg must be set before use */
    ctx->pressure = *ctx->sensor_reg;
}

/* ---- PID controller ----------------------------------------------------- */

/*
 * Discrete PID with:
 *   - Anti-windup via back-calculation
 *   - Derivative-on-measurement (not error) to avoid setpoint kicks
 *   - Output clamped to 0–100% duty
 */

void pid_tick(ventway_ctx_t *ctx)
{
    fp16_t target = ctx->pressure_target[ctx->state];
    fp16_t error  = target - ctx->pressure;

    /* Proportional */
    fp16_t p_term = fp_mul(ctx->kp, error);

    /* Derivative on measurement (negated: rising pressure → negative derivative) */
    fp16_t d_term = -fp_mul(ctx->kd, fp_div(ctx->pressure - ctx->pid_prev_meas, DT_FP));
    ctx->pid_prev_meas = ctx->pressure;

    /* Unsaturated output */
    fp16_t output = p_term + ctx->pid_integral + d_term;

    /* Clamp to 0–100% */
    fp16_t output_sat = output;
    if (output_sat > DUTY_MAX_PCT) output_sat = DUTY_MAX_PCT;
    if (output_sat < 0)        output_sat = 0;

    /* Anti-windup: back-calculation, with leaky integrator (alpha=0.998)
     * so recent errors carry more weight than old ones. */
    fp16_t sat_diff = output_sat - output;
    ctx->pid_integral = fp_mul(PID_ALPHA, ctx->pid_integral)
                      + fp_mul(ctx->ki, fp_mul(error, DT_FP))
                      + fp_mul(ctx->kb, fp_mul(sat_diff, DT_FP));

    ctx->duty_pct = (uint32_t)FP_TO_INT(output_sat);
}

/* ---- State machine ------------------------------------------------------ */

void enter_state(ventway_ctx_t *ctx, state_t s)
{
    if (s >= STATE_COUNT)
        return;

    ctx->state       = s;
    ctx->state_ticks = ctx->duration_ms[s] / TICK_MS;

    if (s == EXHALE) {
        ctx->pid_prev_meas = ctx->pressure;  /* avoid derivative kick */
        ctx->pid_integral  = 0;
        ctx->duty_pct      = 0;
    }

    if (s == INHALE)
        ctx->cycle_count++;

    ctx->state_changed = 1;
}

void state_log(ventway_ctx_t *ctx)
{
    if (!ctx->state_changed)
        return;
    ctx->state_changed = 0;

    tx_puts(ctx, "[cycle ");
    tx_put_uint(ctx, ctx->cycle_count);
    tx_puts(ctx, "] ");
    tx_puts(ctx, state_names[ctx->state]);
    tx_puts(ctx, " \xe2\x80\x94 target ");
    tx_put_fp(ctx, ctx->pressure_target[ctx->state], 0);
    tx_puts(ctx, " cmH2O, P=");
    tx_put_fp(ctx, ctx->pressure, 1);
    tx_puts(ctx, ", duty ");
    tx_put_uint(ctx, ctx->duty_pct);
    tx_puts(ctx, "%\r\n");
}

int state_machine_tick(ventway_ctx_t *ctx)
{
    ctx->tick_count++;

    if (ctx->state_ticks > 0) {
        ctx->state_ticks--;
        /* Closed-loop: read sensor, then PID computes duty */
        sensor_read(ctx);
        pid_tick(ctx);
        return 0;
    }

    switch (ctx->state) {
    case INHALE: enter_state(ctx, HOLD);   break;
    case HOLD:   enter_state(ctx, EXHALE); break;
    case EXHALE: enter_state(ctx, INHALE); break;
    case STATE_COUNT: return 0;  /* unreachable — silences warning */
    }
    return 1;
}
