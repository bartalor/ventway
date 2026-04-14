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
#define TX_BUF_SIZE     128U  /* must be power of 2 */
#define RX_BUF_SIZE     64U   /* must be power of 2 */
#define CMD_BUF_SIZE    32U

/* ---- State machine types ------------------------------------------------ */

typedef enum {
    INHALE,
    HOLD,
    EXHALE,
    STATE_COUNT  /* must be last — used for bounds checking */
} state_t;

extern const char *const state_names[STATE_COUNT];
extern const uint32_t    state_duration_ms[STATE_COUNT];
extern const uint32_t    state_duty_pct[STATE_COUNT];

/* ---- Context struct ----------------------------------------------------- */

typedef struct {
    /* TX ring buffer */
    char     tx_buf[TX_BUF_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;

    /* RX ring buffer (ISR writes, main loop reads) */
    char     rx_buf[RX_BUF_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;

    /* Command line buffer */
    char     cmd_buf[CMD_BUF_SIZE];
    uint32_t cmd_len;

    /* Per-instance configuration (copied from defaults at init) */
    uint32_t duration_ms[STATE_COUNT];
    uint32_t duty_pct_cfg[STATE_COUNT];

    /* State machine */
    state_t  state;
    uint32_t cycle_count;
    uint32_t tick_count;
    uint32_t state_ticks;
    uint32_t duty_pct;
} ventway_ctx_t;

/* ---- TX buffer API ------------------------------------------------------ */

void     ventway_init(ventway_ctx_t *ctx);
void     tx_put(ventway_ctx_t *ctx, char c);
void     tx_puts(ventway_ctx_t *ctx, const char *s);
void     tx_put_uint(ventway_ctx_t *ctx, uint32_t n);
uint32_t tx_read(ventway_ctx_t *ctx, char *dst, uint32_t max_len);

/* ---- RX buffer API ------------------------------------------------------ */

void rx_put(ventway_ctx_t *ctx, char c);
int  rx_get(ventway_ctx_t *ctx, char *out);

/* ---- Command processing API --------------------------------------------- */

void cmd_process_byte(ventway_ctx_t *ctx, char c);
void cmd_execute(ventway_ctx_t *ctx);

/* ---- State machine API -------------------------------------------------- */

void enter_state(ventway_ctx_t *ctx, state_t s);
int  state_machine_tick(ventway_ctx_t *ctx);

#endif /* VENTWAY_H */
