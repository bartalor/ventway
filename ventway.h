/*
 * ventway.h — Testable core logic for the ventilator state machine
 *
 * This header extracts pure logic from main.c so it can be compiled
 * and tested on a host machine without hardware dependencies.
 */

#ifndef VENTWAY_H
#define VENTWAY_H

#include <stdint.h>

/* ---- Constants ---------------------------------------------------------- */

#define SYS_CLK         16000000U
#define TICK_MS         10

/* ---- State machine ------------------------------------------------------ */

typedef enum { INHALE, HOLD, EXHALE } state_t;

static const char *const state_names[] = { "INHALE", "HOLD", "EXHALE" };

static const uint32_t state_duration_ms[] = {
    1000,   /* INHALE */
     500,   /* HOLD   */
    1500,   /* EXHALE */
};

static const uint32_t state_duty_pct[] = {
    80,     /* INHALE */
    30,     /* HOLD   */
     0,     /* EXHALE */
};

/* ---- TX ring buffer ----------------------------------------------------- */

#define TX_BUF_SIZE 128  /* must be power of 2 */

static volatile char     tx_buf[TX_BUF_SIZE];
static volatile uint32_t tx_head;
static volatile uint32_t tx_tail;

static inline void tx_reset(void)
{
    tx_head = 0;
    tx_tail = 0;
}

static inline void tx_put(char c)
{
    uint32_t next = (tx_head + 1) & (TX_BUF_SIZE - 1);
    if (next == tx_tail)
        return;  /* buffer full — drop character */
    tx_buf[tx_head] = c;
    tx_head = next;
}

static inline void tx_puts(const char *s)
{
    while (*s)
        tx_put(*s++);
}

static inline void tx_put_uint(uint32_t n)
{
    char buf[11];
    int i = 0;

    if (n == 0) {
        tx_put('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--)
        tx_put(buf[i]);
}

/* Read the TX buffer contents into a destination string (for testing).
 * Returns number of characters read. */
static inline uint32_t tx_read(char *dst, uint32_t max_len)
{
    uint32_t n = 0;
    while (tx_tail != tx_head && n < max_len - 1) {
        dst[n++] = (char)tx_buf[tx_tail];
        tx_tail = (tx_tail + 1) & (TX_BUF_SIZE - 1);
    }
    dst[n] = '\0';
    return n;
}

/* ---- State machine logic ------------------------------------------------ */

static volatile state_t  current_state = INHALE;
static volatile uint32_t cycle_count   = 0;
static volatile uint32_t tick_count    = 0;
static volatile uint32_t state_ticks   = 0;

/* PWM duty — written by enter_state, read by tests or hardware */
static volatile uint32_t pwm_duty_pct  = 0;

static inline void enter_state(state_t s)
{
    current_state = s;
    state_ticks = state_duration_ms[s] / TICK_MS;
    pwm_duty_pct = state_duty_pct[s];

    if (s == INHALE)
        cycle_count++;

    tx_puts("[cycle ");
    tx_put_uint(cycle_count);
    tx_puts("] ");
    tx_puts(state_names[s]);
    tx_puts(" — duty ");
    tx_put_uint(state_duty_pct[s]);
    tx_puts("%\r\n");
}

/* Advance the state machine by one tick. Returns 1 if a transition happened. */
static inline int state_machine_tick(void)
{
    tick_count++;

    if (state_ticks > 0) {
        state_ticks--;
        return 0;
    }

    switch (current_state) {
    case INHALE: enter_state(HOLD);   break;
    case HOLD:   enter_state(EXHALE); break;
    case EXHALE: enter_state(INHALE); break;
    }
    return 1;
}

/* Reset all state machine globals (for testing) */
static inline void state_machine_reset(void)
{
    current_state = INHALE;
    cycle_count   = 0;
    tick_count    = 0;
    state_ticks   = 0;
    pwm_duty_pct  = 0;
    tx_reset();
}

#endif /* VENTWAY_H */
