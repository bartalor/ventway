/*
 * ventway.c — Ventilator state machine and TX buffer implementation
 */

#include "ventway.h"

/* ---- Compile-time invariants -------------------------------------------- */

_Static_assert((TX_BUF_SIZE & (TX_BUF_SIZE - 1)) == 0,
               "TX_BUF_SIZE must be a power of 2");

_Static_assert(sizeof(state_names) / sizeof(state_names[0]) == STATE_COUNT,
               "state_names must have exactly STATE_COUNT entries");

_Static_assert(sizeof(state_duration_ms) / sizeof(state_duration_ms[0]) == STATE_COUNT,
               "state_duration_ms must have exactly STATE_COUNT entries");

_Static_assert(sizeof(state_duty_pct) / sizeof(state_duty_pct[0]) == STATE_COUNT,
               "state_duty_pct must have exactly STATE_COUNT entries");

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

const uint32_t state_duty_pct[STATE_COUNT] = {
    [INHALE] = 80,
    [HOLD]   = 30,
    [EXHALE] =  0,
};

/* ---- TX ring buffer ----------------------------------------------------- */

/*@ requires \valid(ctx);
    assigns  ctx->tx_head, ctx->tx_tail, ctx->state, ctx->cycle_count,
             ctx->tick_count, ctx->state_ticks, ctx->duty_pct;
    ensures  ctx->tx_head == 0;
    ensures  ctx->tx_tail == 0;
    ensures  ctx->state == INHALE;
    ensures  ctx->cycle_count == 0;
    ensures  ctx->tick_count == 0;
    ensures  ctx->state_ticks == 0;
    ensures  ctx->duty_pct == 0;
*/
void ventway_init(ventway_ctx_t *ctx)
{
    ctx->tx_head    = 0;
    ctx->tx_tail    = 0;
    ctx->state      = INHALE;
    ctx->cycle_count = 0;
    ctx->tick_count  = 0;
    ctx->state_ticks = 0;
    ctx->duty_pct    = 0;
}

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    assigns  ctx->tx_buf[0 .. TX_BUF_SIZE - 1], ctx->tx_head;
    ensures  ctx->tx_head < TX_BUF_SIZE;
*/
void tx_put(ventway_ctx_t *ctx, char c)
{
    uint32_t next = (ctx->tx_head + 1) & (TX_BUF_SIZE - 1);
    if (next == ctx->tx_tail)
        return;  /* buffer full — drop character */
    ctx->tx_buf[ctx->tx_head] = c;
    ctx->tx_head = next;
}

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    requires \valid_read(s) && \valid_read(s + (0 .. \block_length(s) - 1));
    assigns  ctx->tx_buf[0 .. TX_BUF_SIZE - 1], ctx->tx_head;
    ensures  ctx->tx_head < TX_BUF_SIZE;
*/
void tx_puts(ventway_ctx_t *ctx, const char *s)
{
    while (*s)
        tx_put(ctx, *s++);
}

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    assigns  ctx->tx_buf[0 .. TX_BUF_SIZE - 1], ctx->tx_head;
    ensures  ctx->tx_head < TX_BUF_SIZE;
*/
void tx_put_uint(ventway_ctx_t *ctx, uint32_t n)
{
    char buf[11];
    int i = 0;

    if (n == 0) {
        tx_put(ctx, '0');
        return;
    }
    while (n > 0) {
        /*@ assert 0 <= i < 10; */
        buf[i++] = '0' + (char)(n % 10);
        n /= 10;
    }
    while (i--)
        tx_put(ctx, buf[i]);
}

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    requires max_len > 0;
    requires \valid(dst + (0 .. max_len - 1));
    assigns  ctx->tx_tail, dst[0 .. max_len - 1];
    ensures  ctx->tx_tail < TX_BUF_SIZE;
    ensures  \result < max_len;
    ensures  dst[\result] == '\0';
*/
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

/* ---- State machine ------------------------------------------------------ */

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    assigns  ctx->state, ctx->state_ticks, ctx->duty_pct, ctx->cycle_count,
             ctx->tx_buf[0 .. TX_BUF_SIZE - 1], ctx->tx_head;
    ensures  ctx->tx_head < TX_BUF_SIZE;
    ensures  s < STATE_COUNT ==> ctx->state == s;
    ensures  s < STATE_COUNT ==> ctx->duty_pct == state_duty_pct[s];
*/
void enter_state(ventway_ctx_t *ctx, state_t s)
{
    if (s >= STATE_COUNT)
        return;

    ctx->state       = s;
    ctx->state_ticks = state_duration_ms[s] / TICK_MS;
    ctx->duty_pct    = state_duty_pct[s];

    if (s == INHALE)
        ctx->cycle_count++;

    tx_puts(ctx, "[cycle ");
    tx_put_uint(ctx, ctx->cycle_count);
    tx_puts(ctx, "] ");
    tx_puts(ctx, state_names[s]);
    tx_puts(ctx, " \xe2\x80\x94 duty ");
    tx_put_uint(ctx, state_duty_pct[s]);
    tx_puts(ctx, "%\r\n");
}

/*@ requires \valid(ctx);
    requires ctx->tx_head < TX_BUF_SIZE;
    requires ctx->tx_tail < TX_BUF_SIZE;
    requires ctx->state < STATE_COUNT;
    assigns  ctx->tick_count, ctx->state_ticks, ctx->state, ctx->duty_pct,
             ctx->cycle_count, ctx->tx_buf[0 .. TX_BUF_SIZE - 1], ctx->tx_head;
    ensures  ctx->tx_head < TX_BUF_SIZE;
    ensures  ctx->state < STATE_COUNT;
    ensures  \result == 0 || \result == 1;
*/
int state_machine_tick(ventway_ctx_t *ctx)
{
    ctx->tick_count++;

    if (ctx->state_ticks > 0) {
        ctx->state_ticks--;
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
