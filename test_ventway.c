/*
 * test_ventway.c — Native host tests for ventway state machine and TX buffer
 *
 * Build:  make test
 * Run:    ./build/test_ventway
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ventway.h"

/* ---- Minimal test harness ----------------------------------------------- */

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do {                                      \
    tests_run++;                                            \
    printf("  %-50s", #name);                               \
    name();                                                 \
    tests_passed++;                                         \
    printf("PASS\n");                                       \
} while (0)

#define ASSERT(cond) do {                                   \
    if (!(cond)) {                                          \
        printf("FAIL\n    %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1);                                            \
    }                                                       \
} while (0)

#define ASSERT_EQ(a, b) do {                                \
    if ((a) != (b)) {                                       \
        printf("FAIL\n    %s:%d: %s == %u, expected %u\n", \
               __FILE__, __LINE__, #a,                      \
               (unsigned)(a), (unsigned)(b));                \
        exit(1);                                            \
    }                                                       \
} while (0)

#define ASSERT_STR(s, expected) do {                        \
    if (strcmp((s), (expected)) != 0) {                      \
        printf("FAIL\n    %s:%d: got \"%s\", expected \"%s\"\n", \
               __FILE__, __LINE__, (s), (expected));        \
        exit(1);                                            \
    }                                                       \
} while (0)

/* ---- TX buffer tests ---------------------------------------------------- */

TEST(test_tx_put_single_char)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put(&ctx, 'A');
    char out[8];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "A");
}

TEST(test_tx_puts_string)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_puts(&ctx, "hello");
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "hello");
}

TEST(test_tx_put_uint_zero)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put_uint(&ctx, 0);
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "0");
}

TEST(test_tx_put_uint_number)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put_uint(&ctx, 12345);
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "12345");
}

TEST(test_tx_put_uint_large)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put_uint(&ctx, 4294967295U);
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "4294967295");
}

TEST(test_tx_buffer_overflow_drops)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    /* Fill the buffer completely (TX_BUF_SIZE - 1 usable slots) */
    for (unsigned i = 0; i < TX_BUF_SIZE - 1; i++)
        tx_put(&ctx, 'X');
    /* This should be dropped */
    tx_put(&ctx, 'Y');

    char out[TX_BUF_SIZE + 8];
    uint32_t n = tx_read(&ctx, out, sizeof(out));
    ASSERT_EQ(n, TX_BUF_SIZE - 1);
    /* All chars should be 'X', no 'Y' */
    for (uint32_t i = 0; i < n; i++)
        ASSERT_EQ(out[i], 'X');
}

TEST(test_tx_reset_clears_buffer)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_puts(&ctx, "data");
    ventway_init(&ctx);  /* re-init acts as reset */
    char out[8];
    uint32_t n = tx_read(&ctx, out, sizeof(out));
    ASSERT_EQ(n, 0);
    ASSERT_STR(out, "");
}

/* ---- State machine constants tests -------------------------------------- */

TEST(test_state_names)
{
    ASSERT_STR(state_names[INHALE], "INHALE");
    ASSERT_STR(state_names[HOLD],   "HOLD");
    ASSERT_STR(state_names[EXHALE], "EXHALE");
}

TEST(test_state_durations)
{
    ASSERT_EQ(state_duration_ms[INHALE], 1000);
    ASSERT_EQ(state_duration_ms[HOLD],    500);
    ASSERT_EQ(state_duration_ms[EXHALE], 1500);
}

TEST(test_total_cycle_duration)
{
    uint32_t total = state_duration_ms[INHALE]
                   + state_duration_ms[HOLD]
                   + state_duration_ms[EXHALE];
    ASSERT_EQ(total, 3000);  /* 3s = 20 breaths/min */
}

TEST(test_state_duty_cycles)
{
    ASSERT_EQ(state_duty_pct[INHALE], 80);
    ASSERT_EQ(state_duty_pct[HOLD],   30);
    ASSERT_EQ(state_duty_pct[EXHALE],  0);
}

/* ---- Runtime configuration tests ---------------------------------------- */

TEST(test_init_copies_default_durations)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ASSERT_EQ(ctx.duration_ms[INHALE], 1000);
    ASSERT_EQ(ctx.duration_ms[HOLD],    500);
    ASSERT_EQ(ctx.duration_ms[EXHALE], 1500);
}

TEST(test_init_copies_default_duty_cycles)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ASSERT_EQ(ctx.duty_pct_cfg[INHALE], 80);
    ASSERT_EQ(ctx.duty_pct_cfg[HOLD],   30);
    ASSERT_EQ(ctx.duty_pct_cfg[EXHALE],  0);
}

TEST(test_custom_duration_applied)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.duration_ms[INHALE] = 2000;
    enter_state(&ctx, INHALE);
    ASSERT_EQ(ctx.state_ticks, 2000 / TICK_MS);
}

TEST(test_custom_duty_applied)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.duty_pct_cfg[HOLD] = 50;
    enter_state(&ctx, HOLD);
    ASSERT_EQ(ctx.duty_pct, 50);
}

/* ---- enter_state tests -------------------------------------------------- */

TEST(test_enter_state_inhale)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.state_ticks, 1000 / TICK_MS);
    ASSERT_EQ(ctx.duty_pct, 80);
    ASSERT_EQ(ctx.cycle_count, 1);  /* INHALE increments cycle */
    ASSERT_EQ(ctx.state_changed, 1);
}

TEST(test_enter_state_hold)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, HOLD);

    ASSERT_EQ(ctx.state, HOLD);
    ASSERT_EQ(ctx.state_ticks, 500 / TICK_MS);
    ASSERT_EQ(ctx.duty_pct, 30);
    ASSERT_EQ(ctx.cycle_count, 0);  /* HOLD does not increment */
}

TEST(test_enter_state_exhale)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, EXHALE);

    ASSERT_EQ(ctx.state, EXHALE);
    ASSERT_EQ(ctx.state_ticks, 1500 / TICK_MS);
    ASSERT_EQ(ctx.duty_pct, 0);
    ASSERT_EQ(ctx.cycle_count, 0);  /* EXHALE does not increment */
}

TEST(test_state_log_message)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    state_log(&ctx);

    ASSERT_EQ(ctx.state_changed, 0);  /* flag cleared */
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));
    /* Should contain cycle number, state name, and duty */
    ASSERT(strstr(out, "[cycle 1]") != NULL);
    ASSERT(strstr(out, "INHALE") != NULL);
    ASSERT(strstr(out, "80%") != NULL);
}

TEST(test_state_log_noop_when_no_change)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    /* No enter_state — flag is 0 */
    state_log(&ctx);
    char out[TX_BUF_SIZE];
    uint32_t n = tx_read(&ctx, out, sizeof(out));
    ASSERT_EQ(n, 0);
}

TEST(test_enter_state_invalid_is_noop)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, STATE_COUNT);

    ASSERT_EQ(ctx.state, INHALE);  /* unchanged from init */
    ASSERT_EQ(ctx.cycle_count, 0);
    ASSERT_EQ(ctx.duty_pct, 0);
}

/* ---- State machine tick tests ------------------------------------------- */

TEST(test_tick_decrements_state_ticks)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    uint32_t initial = ctx.state_ticks;

    state_machine_tick(&ctx);

    ASSERT_EQ(ctx.state_ticks, initial - 1);
    ASSERT_EQ(ctx.state, INHALE);  /* no transition yet */
}

TEST(test_tick_no_transition_returns_zero)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    int transitioned = state_machine_tick(&ctx);
    ASSERT_EQ(transitioned, 0);
}

TEST(test_tick_transition_returns_one)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.state_ticks = 0;  /* force immediate transition */

    int transitioned = state_machine_tick(&ctx);
    ASSERT_EQ(transitioned, 1);
}

TEST(test_full_cycle_inhale_hold_exhale_inhale)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.cycle_count, 1);

    /* Tick through INHALE: state_ticks=100, need 100 to drain + 1 to transition */
    for (int i = 0; i < 101; i++)
        state_machine_tick(&ctx);

    /* Should have transitioned to HOLD */
    ASSERT_EQ(ctx.state, HOLD);
    ASSERT_EQ(ctx.duty_pct, 30);

    /* Tick through HOLD: state_ticks=50, need 51 */
    for (int i = 0; i < 51; i++)
        state_machine_tick(&ctx);

    /* Should have transitioned to EXHALE */
    ASSERT_EQ(ctx.state, EXHALE);
    ASSERT_EQ(ctx.duty_pct, 0);

    /* Tick through EXHALE: state_ticks=150, need 151 */
    for (int i = 0; i < 151; i++)
        state_machine_tick(&ctx);

    /* Should have transitioned back to INHALE */
    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.duty_pct, 80);
    ASSERT_EQ(ctx.cycle_count, 2);  /* second cycle */
}

TEST(test_multiple_cycles)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    /* Run 5 full cycles (303 ticks per cycle: 101+51+151) */
    for (int c = 0; c < 5; c++)
        for (int t = 0; t < 303; t++)
            state_machine_tick(&ctx);

    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.cycle_count, 6);  /* initial + 5 transitions back */
}

TEST(test_tick_count_increments)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    uint32_t before = ctx.tick_count;

    for (int i = 0; i < 10; i++)
        state_machine_tick(&ctx);

    ASSERT_EQ(ctx.tick_count, before + 10);
}

/* ---- RX buffer tests ---------------------------------------------------- */

TEST(test_rx_put_get)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    rx_put(&ctx, 'A');
    char c;
    ASSERT(rx_get(&ctx, &c));
    ASSERT_EQ(c, 'A');
}

TEST(test_rx_get_empty_returns_zero)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char c;
    ASSERT_EQ(rx_get(&ctx, &c), 0);
}

TEST(test_rx_overflow_drops)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    for (unsigned i = 0; i < RX_BUF_SIZE - 1; i++)
        rx_put(&ctx, 'X');
    rx_put(&ctx, 'Y');  /* should be dropped */
    char c;
    for (unsigned i = 0; i < RX_BUF_SIZE - 1; i++) {
        ASSERT(rx_get(&ctx, &c));
        ASSERT_EQ(c, 'X');
    }
    ASSERT_EQ(rx_get(&ctx, &c), 0);  /* no 'Y' */
}

/* ---- Command processing tests ------------------------------------------- */

static void feed_cmd(ventway_ctx_t *ctx, const char *cmd)
{
    while (*cmd)
        cmd_process_byte(ctx, *cmd++);
    cmd_process_byte(ctx, '\n');
}

TEST(test_cmd_status)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    /* Drain any init output */
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "status");
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "INHALE") != NULL);
    ASSERT(strstr(out, "HOLD") != NULL);
    ASSERT(strstr(out, "EXHALE") != NULL);
    ASSERT(strstr(out, "1000ms") != NULL);
    ASSERT(strstr(out, "80%") != NULL);
}

TEST(test_cmd_set_duration)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "inhale 2000");
    ASSERT_EQ(ctx.duration_ms[INHALE], 2000);
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "2000") != NULL);
}

TEST(test_cmd_set_duty)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "duty hold 50");
    ASSERT_EQ(ctx.duty_pct_cfg[HOLD], 50);
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "50%") != NULL);
}

TEST(test_cmd_bad_value)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "inhale abc");
    ASSERT_EQ(ctx.duration_ms[INHALE], 1000);  /* unchanged */
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "bad value") != NULL);
}

TEST(test_cmd_unknown)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "foo");
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "unknown cmd") != NULL);
}

TEST(test_cmd_empty_line_ignored)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    cmd_process_byte(&ctx, '\n');
    cmd_process_byte(&ctx, '\r');
    uint32_t n = tx_read(&ctx, out, sizeof(out));
    ASSERT_EQ(n, 0);
}

TEST(test_cmd_overflow_value)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "inhale 99999999999");
    ASSERT_EQ(ctx.duration_ms[INHALE], 1000);  /* unchanged */
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "bad value") != NULL);
}

TEST(test_cmd_duty_over_100)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "duty inhale 101");
    ASSERT_EQ(ctx.duty_pct_cfg[INHALE], 80);  /* unchanged */
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "bad value") != NULL);
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    printf("Running ventway tests...\n\n");

    printf("TX buffer:\n");
    RUN(test_tx_put_single_char);
    RUN(test_tx_puts_string);
    RUN(test_tx_put_uint_zero);
    RUN(test_tx_put_uint_number);
    RUN(test_tx_put_uint_large);
    RUN(test_tx_buffer_overflow_drops);
    RUN(test_tx_reset_clears_buffer);

    printf("\nState machine constants:\n");
    RUN(test_state_names);
    RUN(test_state_durations);
    RUN(test_total_cycle_duration);
    RUN(test_state_duty_cycles);

    printf("\nRuntime configuration:\n");
    RUN(test_init_copies_default_durations);
    RUN(test_init_copies_default_duty_cycles);
    RUN(test_custom_duration_applied);
    RUN(test_custom_duty_applied);

    printf("\nenter_state:\n");
    RUN(test_enter_state_inhale);
    RUN(test_enter_state_hold);
    RUN(test_enter_state_exhale);
    RUN(test_state_log_message);
    RUN(test_state_log_noop_when_no_change);
    RUN(test_enter_state_invalid_is_noop);

    printf("\nState machine ticks:\n");
    RUN(test_tick_decrements_state_ticks);
    RUN(test_tick_no_transition_returns_zero);
    RUN(test_tick_transition_returns_one);
    RUN(test_full_cycle_inhale_hold_exhale_inhale);
    RUN(test_multiple_cycles);
    RUN(test_tick_count_increments);

    printf("\nRX buffer:\n");
    RUN(test_rx_put_get);
    RUN(test_rx_get_empty_returns_zero);
    RUN(test_rx_overflow_drops);

    printf("\nCommand processing:\n");
    RUN(test_cmd_status);
    RUN(test_cmd_set_duration);
    RUN(test_cmd_set_duty);
    RUN(test_cmd_bad_value);
    RUN(test_cmd_unknown);
    RUN(test_cmd_empty_line_ignored);
    RUN(test_cmd_overflow_value);
    RUN(test_cmd_duty_over_100);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return 0;
}
