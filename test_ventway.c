/*
 * test_ventway.c — Native host tests for ventway state machine and TX buffer
 *
 * Compile with: cc -std=c99 -Wall -Wextra -o test_ventway test_ventway.c
 * Run with:     ./test_ventway
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
    tx_reset();
    tx_put('A');
    char out[8];
    tx_read(out, sizeof(out));
    ASSERT_STR(out, "A");
}

TEST(test_tx_puts_string)
{
    tx_reset();
    tx_puts("hello");
    char out[16];
    tx_read(out, sizeof(out));
    ASSERT_STR(out, "hello");
}

TEST(test_tx_put_uint_zero)
{
    tx_reset();
    tx_put_uint(0);
    char out[16];
    tx_read(out, sizeof(out));
    ASSERT_STR(out, "0");
}

TEST(test_tx_put_uint_number)
{
    tx_reset();
    tx_put_uint(12345);
    char out[16];
    tx_read(out, sizeof(out));
    ASSERT_STR(out, "12345");
}

TEST(test_tx_put_uint_large)
{
    tx_reset();
    tx_put_uint(4294967295U);
    char out[16];
    tx_read(out, sizeof(out));
    ASSERT_STR(out, "4294967295");
}

TEST(test_tx_buffer_overflow_drops)
{
    tx_reset();
    /* Fill the buffer completely (TX_BUF_SIZE - 1 usable slots) */
    for (int i = 0; i < TX_BUF_SIZE - 1; i++)
        tx_put('X');
    /* This should be dropped */
    tx_put('Y');

    char out[TX_BUF_SIZE + 8];
    uint32_t n = tx_read(out, sizeof(out));
    ASSERT_EQ(n, TX_BUF_SIZE - 1);
    /* All chars should be 'X', no 'Y' */
    for (uint32_t i = 0; i < n; i++)
        ASSERT_EQ(out[i], 'X');
}

TEST(test_tx_reset_clears_buffer)
{
    tx_reset();
    tx_puts("data");
    tx_reset();
    char out[8];
    uint32_t n = tx_read(out, sizeof(out));
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

/* ---- enter_state tests -------------------------------------------------- */

TEST(test_enter_state_inhale)
{
    state_machine_reset();
    enter_state(INHALE);

    ASSERT_EQ(current_state, INHALE);
    ASSERT_EQ(state_ticks, 1000 / TICK_MS);
    ASSERT_EQ(pwm_duty_pct, 80);
    ASSERT_EQ(cycle_count, 1);  /* INHALE increments cycle */
}

TEST(test_enter_state_hold)
{
    state_machine_reset();
    enter_state(HOLD);

    ASSERT_EQ(current_state, HOLD);
    ASSERT_EQ(state_ticks, 500 / TICK_MS);
    ASSERT_EQ(pwm_duty_pct, 30);
    ASSERT_EQ(cycle_count, 0);  /* HOLD does not increment */
}

TEST(test_enter_state_exhale)
{
    state_machine_reset();
    enter_state(EXHALE);

    ASSERT_EQ(current_state, EXHALE);
    ASSERT_EQ(state_ticks, 1500 / TICK_MS);
    ASSERT_EQ(pwm_duty_pct, 0);
    ASSERT_EQ(cycle_count, 0);  /* EXHALE does not increment */
}

TEST(test_enter_state_logs_message)
{
    state_machine_reset();
    enter_state(INHALE);

    char out[TX_BUF_SIZE];
    tx_read(out, sizeof(out));
    /* Should contain cycle number, state name, and duty */
    ASSERT(strstr(out, "[cycle 1]") != NULL);
    ASSERT(strstr(out, "INHALE") != NULL);
    ASSERT(strstr(out, "80%") != NULL);
}

/* ---- State machine tick tests ------------------------------------------- */

TEST(test_tick_decrements_state_ticks)
{
    state_machine_reset();
    enter_state(INHALE);
    uint32_t initial = state_ticks;

    state_machine_tick();

    ASSERT_EQ(state_ticks, initial - 1);
    ASSERT_EQ(current_state, INHALE);  /* no transition yet */
}

TEST(test_tick_no_transition_returns_zero)
{
    state_machine_reset();
    enter_state(INHALE);

    int transitioned = state_machine_tick();
    ASSERT_EQ(transitioned, 0);
}

TEST(test_tick_transition_returns_one)
{
    state_machine_reset();
    enter_state(INHALE);
    state_ticks = 0;  /* force immediate transition */

    int transitioned = state_machine_tick();
    ASSERT_EQ(transitioned, 1);
}

TEST(test_full_cycle_inhale_hold_exhale_inhale)
{
    state_machine_reset();
    enter_state(INHALE);
    ASSERT_EQ(current_state, INHALE);
    ASSERT_EQ(cycle_count, 1);

    /* Tick through INHALE: state_ticks=100, need 100 to drain + 1 to transition */
    for (int i = 0; i < 101; i++)
        state_machine_tick();

    /* Should have transitioned to HOLD */
    ASSERT_EQ(current_state, HOLD);
    ASSERT_EQ(pwm_duty_pct, 30);

    /* Tick through HOLD: state_ticks=50, need 51 */
    for (int i = 0; i < 51; i++)
        state_machine_tick();

    /* Should have transitioned to EXHALE */
    ASSERT_EQ(current_state, EXHALE);
    ASSERT_EQ(pwm_duty_pct, 0);

    /* Tick through EXHALE: state_ticks=150, need 151 */
    for (int i = 0; i < 151; i++)
        state_machine_tick();

    /* Should have transitioned back to INHALE */
    ASSERT_EQ(current_state, INHALE);
    ASSERT_EQ(pwm_duty_pct, 80);
    ASSERT_EQ(cycle_count, 2);  /* second cycle */
}

TEST(test_multiple_cycles)
{
    state_machine_reset();
    enter_state(INHALE);

    /* Run 5 full cycles (303 ticks per cycle: 101+51+151) */
    for (int c = 0; c < 5; c++)
        for (int t = 0; t < 303; t++)
            state_machine_tick();

    ASSERT_EQ(current_state, INHALE);
    ASSERT_EQ(cycle_count, 6);  /* initial + 5 transitions back */
}

TEST(test_tick_count_increments)
{
    state_machine_reset();
    enter_state(INHALE);
    uint32_t before = tick_count;

    for (int i = 0; i < 10; i++)
        state_machine_tick();

    ASSERT_EQ(tick_count, before + 10);
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

    printf("\nenter_state:\n");
    RUN(test_enter_state_inhale);
    RUN(test_enter_state_hold);
    RUN(test_enter_state_exhale);
    RUN(test_enter_state_logs_message);

    printf("\nState machine ticks:\n");
    RUN(test_tick_decrements_state_ticks);
    RUN(test_tick_no_transition_returns_zero);
    RUN(test_tick_transition_returns_one);
    RUN(test_full_cycle_inhale_hold_exhale_inhale);
    RUN(test_multiple_cycles);
    RUN(test_tick_count_increments);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return 0;
}
