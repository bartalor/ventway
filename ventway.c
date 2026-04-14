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

_Static_assert(sizeof(state_duty_start) / sizeof(state_duty_start[0]) == STATE_COUNT,
               "state_duty_start must have exactly STATE_COUNT entries");

_Static_assert(sizeof(state_duty_end) / sizeof(state_duty_end[0]) == STATE_COUNT,
               "state_duty_end must have exactly STATE_COUNT entries");

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

const uint32_t state_duty_start[STATE_COUNT] = {
    [INHALE] =  0,
    [HOLD]   = 80,
    [EXHALE] = 80,
};

const uint32_t state_duty_end[STATE_COUNT] = {
    [INHALE] = 80,
    [HOLD]   = 80,
    [EXHALE] =  0,
};

/* ---- TX ring buffer ----------------------------------------------------- */

void ventway_init(ventway_ctx_t *ctx)
{
    ctx->tx_head    = 0;
    ctx->tx_tail    = 0;
    ctx->rx_head    = 0;
    ctx->rx_tail    = 0;
    ctx->cmd_len    = 0;
    ctx->state      = INHALE;
    ctx->cycle_count = 0;
    ctx->tick_count  = 0;
    ctx->state_ticks   = 0;
    ctx->duty_pct      = 0;
    ctx->state_changed = 0;

    ctx->state_total_ticks = 0;

    for (int i = 0; i < STATE_COUNT; i++) {
        ctx->duration_ms[i] = state_duration_ms[i];
        ctx->duty_start[i]  = state_duty_start[i];
        ctx->duty_end[i]    = state_duty_end[i];
    }
}

void tx_put(ventway_ctx_t *ctx, char c)
{
    uint32_t next = (ctx->tx_head + 1) & (TX_BUF_SIZE - 1);
    if (next == ctx->tx_tail)
        return;  /* buffer full — drop character */
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
            tx_puts(ctx, "ms duty ");
            tx_put_uint(ctx, ctx->duty_start[i]);
            tx_puts(ctx, "%->");
            tx_put_uint(ctx, ctx->duty_end[i]);
            tx_puts(ctx, "%\r\n");
        }
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
        ctx->duration_ms[st] = val;
        tx_puts(ctx, state_names[st]);
        tx_puts(ctx, " = ");
        tx_put_uint(ctx, val);
        tx_puts(ctx, "ms\r\n");
        return;
    }

    /* "duty inhale 0 80" — set duty ramp start and end */
    if (str_eq(verb, "duty")) {
        char *state_arg, *start_arg, *end_arg;
        if (!next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &state_arg) ||
            !next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &start_arg) ||
            !next_token(ctx->cmd_buf, ctx->cmd_len, &pos, &end_arg)) {
            tx_puts(ctx, "usage: duty <state> <start%> <end%>\r\n");
            return;
        }
        state_t dst = parse_state_name(state_arg);
        if (dst == STATE_COUNT) {
            tx_puts(ctx, "bad state\r\n");
            return;
        }
        int ok1, ok2;
        uint32_t s_pct = parse_uint(start_arg, &ok1);
        uint32_t e_pct = parse_uint(end_arg, &ok2);
        if (!ok1 || !ok2 || s_pct > 100 || e_pct > 100) {
            tx_puts(ctx, "bad value (0-100)\r\n");
            return;
        }
        ctx->duty_start[dst] = s_pct;
        ctx->duty_end[dst]   = e_pct;
        tx_puts(ctx, state_names[dst]);
        tx_puts(ctx, " duty = ");
        tx_put_uint(ctx, s_pct);
        tx_puts(ctx, "%->");
        tx_put_uint(ctx, e_pct);
        tx_puts(ctx, "%\r\n");
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

/* ---- State machine ------------------------------------------------------ */

void enter_state(ventway_ctx_t *ctx, state_t s)
{
    if (s >= STATE_COUNT)
        return;

    ctx->state       = s;
    ctx->state_ticks = ctx->duration_ms[s] / TICK_MS;
    ctx->state_total_ticks = ctx->state_ticks;
    ctx->duty_pct          = ctx->duty_start[s];

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
    tx_puts(ctx, " \xe2\x80\x94 duty ");
    tx_put_uint(ctx, ctx->duty_start[ctx->state]);
    tx_puts(ctx, "%->");
    tx_put_uint(ctx, ctx->duty_end[ctx->state]);
    tx_puts(ctx, "%\r\n");
}

static uint32_t interpolate_duty(uint32_t start, uint32_t end,
                                  uint32_t elapsed, uint32_t total)
{
    if (total == 0)
        return end;
    if (end >= start)
        return start + ((end - start) * elapsed) / total;
    else
        return start - ((start - end) * elapsed) / total;
}

int state_machine_tick(ventway_ctx_t *ctx)
{
    ctx->tick_count++;

    if (ctx->state_ticks > 0) {
        ctx->state_ticks--;
        uint32_t elapsed = ctx->state_total_ticks - ctx->state_ticks;
        ctx->duty_pct = interpolate_duty(
            ctx->duty_start[ctx->state], ctx->duty_end[ctx->state],
            elapsed, ctx->state_total_ticks);
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
