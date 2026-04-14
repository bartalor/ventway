/*
 * test_ventway.c — Native host tests for ventway state machine, PID, and lung model
 *
 * Build:  make test
 * Run:    ./build/test_ventway
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include "ventway.h"

/* ---- Minimal test harness ----------------------------------------------- */

static int tests_run    = 0;
static int tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do {                                      \
    tests_run++;                                            \
    printf("  %-55s", #name);                               \
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

/* Assert fixed-point value is within tolerance of expected integer */
#define ASSERT_FP_NEAR(val, expected_int, tolerance_int) do {   \
    fp16_t _v = (val);                                          \
    fp16_t _lo = FP_FROM_INT((expected_int) - (tolerance_int)); \
    fp16_t _hi = FP_FROM_INT((expected_int) + (tolerance_int)); \
    if (_v < _lo || _v > _hi) {                                 \
        printf("FAIL\n    %s:%d: %s = %d.%02d, expected %d +/- %d\n", \
               __FILE__, __LINE__, #val,                        \
               (int)(_v >> FP_SHIFT),                           \
               (int)(((_v & (FP_ONE-1)) * 100) >> FP_SHIFT),   \
               (expected_int), (tolerance_int));                \
        exit(1);                                                \
    }                                                           \
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
    ASSERT_EQ(ctx.tx_overflow, 0);
    /* Fill the buffer completely (TX_BUF_SIZE - 1 usable slots) */
    for (unsigned i = 0; i < TX_BUF_SIZE - 1; i++)
        tx_put(&ctx, 'X');
    /* This should be dropped and counted */
    tx_put(&ctx, 'Y');
    ASSERT_EQ(ctx.tx_overflow, 1);

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

/* ---- Fixed-point math tests --------------------------------------------- */

TEST(test_fp_mul_basic)
{
    fp16_t a = FP_FROM_INT(3);
    fp16_t b = FP_FROM_INT(4);
    ASSERT_EQ(fp_mul(a, b), FP_FROM_INT(12));
}

TEST(test_fp_mul_fractional)
{
    fp16_t half = FP_ONE / 2;
    fp16_t result = fp_mul(half, half);
    /* 0.5 * 0.5 = 0.25 = FP_ONE/4 */
    ASSERT_EQ(result, FP_ONE / 4);
}

TEST(test_fp_div_basic)
{
    fp16_t a = FP_FROM_INT(10);
    fp16_t b = FP_FROM_INT(2);
    ASSERT_EQ(fp_div(a, b), FP_FROM_INT(5));
}

TEST(test_fp_div_fractional)
{
    fp16_t a = FP_FROM_INT(1);
    fp16_t b = FP_FROM_INT(3);
    fp16_t result = fp_div(a, b);
    /* 1/3 ≈ 0.333... Should be close to FP_ONE/3 */
    fp16_t expected = FP_ONE / 3;
    int32_t diff = result - expected;
    if (diff < 0) diff = -diff;
    ASSERT(diff <= 1);  /* rounding tolerance */
}

TEST(test_fp_div_by_zero_saturates)
{
    ASSERT_EQ(fp_div(FP_FROM_INT(10), 0), INT32_MAX);
    ASSERT_EQ(fp_div(FP_FROM_INT(-10), 0), INT32_MIN);
    ASSERT_EQ(fp_div(0, 0), INT32_MAX);  /* 0 >= 0 */
}

TEST(test_fp_negative)
{
    fp16_t a = FP_FROM_INT(-5);
    fp16_t b = FP_FROM_INT(3);
    fp16_t result = fp_mul(a, b);
    ASSERT_EQ(result, FP_FROM_INT(-15));
}

TEST(test_tx_put_fp)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put_fp(&ctx, FP_FROM_INT(12) + FP_ONE / 2, 1);  /* 12.5 */
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "12.5");
}

TEST(test_tx_put_fp_negative)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    tx_put_fp(&ctx, FP_FROM_INT(-3), 0);
    char out[16];
    tx_read(&ctx, out, sizeof(out));
    ASSERT_STR(out, "-3");
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

TEST(test_default_pressure_targets)
{
    ASSERT_EQ(default_pressure_target[INHALE], FP_FROM_INT(20));
    ASSERT_EQ(default_pressure_target[HOLD],   FP_FROM_INT(20));
    ASSERT_EQ(default_pressure_target[EXHALE], FP_FROM_INT(5));
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

TEST(test_init_copies_default_pressure_targets)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ASSERT_EQ(ctx.pressure_target[INHALE], FP_FROM_INT(20));
    ASSERT_EQ(ctx.pressure_target[HOLD],   FP_FROM_INT(20));
    ASSERT_EQ(ctx.pressure_target[EXHALE], FP_FROM_INT(5));
}

TEST(test_init_lung_defaults)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ASSERT_EQ(ctx.compliance, FP_FROM_INT(50));
    ASSERT_EQ(ctx.resistance, FP_FROM_INT(5));
    ASSERT_EQ(ctx.k_turb,    FP_FROM_INT(10));
    ASSERT_EQ(ctx.lung_volume, 0);
    ASSERT_EQ(ctx.pressure, 0);
}

TEST(test_init_pid_defaults)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ASSERT_EQ(ctx.kp, FP_FROM_INT(3));
    ASSERT_EQ(ctx.ki, FP_ONE);
    ASSERT_EQ(ctx.kd, FP_ONE / 10);
    ASSERT_EQ(ctx.kb, FP_FROM_INT(2));
    ASSERT_EQ(ctx.pid_integral, 0);
    ASSERT_EQ(ctx.pid_prev_meas, 0);
}

TEST(test_custom_duration_applied)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.duration_ms[INHALE] = 2000;
    enter_state(&ctx, INHALE);
    ASSERT_EQ(ctx.state_ticks, 2000 / TICK_MS);
}

/* ---- enter_state tests -------------------------------------------------- */

TEST(test_enter_state_inhale)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.state_ticks, 1000 / TICK_MS);
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
    ASSERT_EQ(ctx.cycle_count, 0);  /* HOLD does not increment */
}

TEST(test_enter_state_exhale)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, EXHALE);

    ASSERT_EQ(ctx.state, EXHALE);
    ASSERT_EQ(ctx.state_ticks, 1500 / TICK_MS);
    ASSERT_EQ(ctx.cycle_count, 0);  /* EXHALE does not increment */
    ASSERT_EQ(ctx.pid_integral, 0);  /* integrator reset on exhale */
    ASSERT_EQ(ctx.duty_pct, 0);     /* duty zeroed on exhale */
}

TEST(test_enter_state_inhale_resets_volume_to_peep)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.lung_volume = FP_FROM_INT(500);  /* arbitrary */
    enter_state(&ctx, INHALE);
    /* Volume should reset to C * PEEP = 50 * 5 = 250 mL */
    ASSERT_EQ(ctx.lung_volume, fp_mul(ctx.compliance, ctx.pressure_target[EXHALE]));
}

TEST(test_enter_state_exhale_resets_integrator)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.pid_integral = FP_FROM_INT(42);
    enter_state(&ctx, EXHALE);
    ASSERT_EQ(ctx.pid_integral, 0);
}

TEST(test_enter_state_hold_preserves_integrator)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.pid_integral = FP_FROM_INT(42);
    enter_state(&ctx, HOLD);
    ASSERT_EQ(ctx.pid_integral, FP_FROM_INT(42));
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
    /* Should contain cycle number, state name, and target pressure */
    ASSERT(strstr(out, "[cycle 1]") != NULL);
    ASSERT(strstr(out, "INHALE") != NULL);
    ASSERT(strstr(out, "target 20 cmH2O") != NULL);
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
}

/* ---- Lung model tests --------------------------------------------------- */

TEST(test_lung_zero_duty_zero_pressure)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.duty_pct = 0;
    ctx.lung_volume = fp_mul(ctx.compliance, ctx.pressure_target[EXHALE]);
    lung_model_tick(&ctx);
    /* With zero duty during inhale, no flow added; volume stays at PEEP level */
    ASSERT_FP_NEAR(ctx.pressure, 5, 1);
}

TEST(test_lung_pressure_rises_with_duty)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.lung_volume = fp_mul(ctx.compliance, ctx.pressure_target[EXHALE]);

    /* Apply high duty for several ticks */
    ctx.duty_pct = 80;
    for (int i = 0; i < 50; i++)
        lung_model_tick(&ctx);

    /* Pressure should have risen above PEEP */
    ASSERT(ctx.pressure > FP_FROM_INT(5));
}

TEST(test_lung_exhale_pressure_decays)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);

    /* Set up some lung volume */
    ctx.lung_volume = FP_FROM_INT(500);
    enter_state(&ctx, EXHALE);

    fp16_t initial_pressure = fp_div(ctx.lung_volume, ctx.compliance);

    /* Tick exhale for a while */
    for (int i = 0; i < 100; i++)
        lung_model_tick(&ctx);

    /* Pressure should have decreased toward PEEP */
    ASSERT(ctx.pressure < initial_pressure);
    ASSERT_FP_NEAR(ctx.pressure, 5, 2);  /* should be near PEEP */
}

TEST(test_lung_volume_clamped_at_max)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.lung_volume = FP_FROM_INT(999);
    ctx.duty_pct = 100;
    lung_model_tick(&ctx);
    ASSERT(ctx.lung_volume <= FP_FROM_INT(1000));
}

TEST(test_lung_peep_valve_clamp)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, EXHALE);
    ctx.lung_volume = FP_FROM_INT(100);  /* low volume */

    /* Tick many times — volume shouldn't drop below C*PEEP */
    for (int i = 0; i < 500; i++)
        lung_model_tick(&ctx);

    fp16_t min_vol = fp_mul(ctx.compliance, ctx.pressure_target[EXHALE]);
    ASSERT(ctx.lung_volume >= min_vol);
}

/* ---- PID controller tests ----------------------------------------------- */

TEST(test_pid_positive_error_increases_duty)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.pressure = 0;  /* target=20, error=+20 → duty should rise */
    ctx.duty_pct = 0;

    pid_tick(&ctx);

    ASSERT(ctx.duty_pct > 0);
}

TEST(test_pid_clamp_max)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.pressure = 0;            /* huge positive error */
    ctx.kp = FP_FROM_INT(100);  /* very high gain */

    pid_tick(&ctx);

    ASSERT(ctx.duty_pct <= 100);
}

TEST(test_pid_clamp_min)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.pressure = FP_FROM_INT(40);  /* way above target=20 → negative error */
    ctx.kp = FP_FROM_INT(100);

    pid_tick(&ctx);

    ASSERT_EQ(ctx.duty_pct, 0);
}

TEST(test_pid_anti_windup)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);
    ctx.pressure = 0;  /* large error, output will saturate */

    /* Run PID saturated for many ticks */
    for (int i = 0; i < 200; i++)
        pid_tick(&ctx);

    /* Now set pressure above target — duty should drop quickly */
    ctx.pressure = FP_FROM_INT(25);
    for (int i = 0; i < 20; i++)
        pid_tick(&ctx);

    /* With anti-windup, integrator shouldn't hold duty high for long */
    ASSERT(ctx.duty_pct < 50);
}

/* ---- Closed-loop integration tests -------------------------------------- */

TEST(test_closed_loop_reaches_target)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    /* Run full closed loop (PID + lung) for 2 seconds (200 ticks) */
    for (int i = 0; i < 200; i++) {
        pid_tick(&ctx);
        lung_model_tick(&ctx);
    }

    /* Pressure should be near 20 cmH2O target */
    ASSERT_FP_NEAR(ctx.pressure, 20, 3);
}

TEST(test_closed_loop_exhale_settles_to_peep)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);

    /* First fill the lungs */
    enter_state(&ctx, INHALE);
    for (int i = 0; i < 100; i++) {
        pid_tick(&ctx);
        lung_model_tick(&ctx);
    }

    /* Now exhale */
    enter_state(&ctx, EXHALE);
    for (int i = 0; i < 300; i++) {
        pid_tick(&ctx);
        lung_model_tick(&ctx);
    }

    /* Pressure should settle near PEEP (5 cmH2O) */
    ASSERT_FP_NEAR(ctx.pressure, 5, 2);
}

TEST(test_closed_loop_full_cycle)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    enter_state(&ctx, INHALE);

    /* Run through a complete cycle using state_machine_tick */
    /* INHALE: 100 ticks + 1 transition */
    for (int i = 0; i < 101; i++)
        state_machine_tick(&ctx);
    ASSERT_EQ(ctx.state, HOLD);

    /* HOLD: 50 ticks + 1 transition */
    for (int i = 0; i < 51; i++)
        state_machine_tick(&ctx);
    ASSERT_EQ(ctx.state, EXHALE);

    /* EXHALE: 150 ticks + 1 transition */
    for (int i = 0; i < 151; i++)
        state_machine_tick(&ctx);
    ASSERT_EQ(ctx.state, INHALE);
    ASSERT_EQ(ctx.cycle_count, 2);
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
    ASSERT(strstr(out, "target=20cmH2O") != NULL);
    ASSERT(strstr(out, "compliance=50") != NULL);
    ASSERT(strstr(out, "Kp=3.0") != NULL);
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

TEST(test_cmd_duration_not_multiple_of_tick)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "inhale 15");
    ASSERT_EQ(ctx.duration_ms[INHALE], 1000);  /* unchanged */
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "must be multiple of") != NULL);
}

TEST(test_cmd_set_target)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "target inhale 25");
    ASSERT_EQ(ctx.pressure_target[INHALE], FP_FROM_INT(25));
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "25") != NULL);
    ASSERT(strstr(out, "cmH2O") != NULL);
}

TEST(test_cmd_target_bad_state)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "target foo 25");
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "bad state") != NULL);
}

TEST(test_cmd_target_over_50)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "target inhale 51");
    ASSERT_EQ(ctx.pressure_target[INHALE], FP_FROM_INT(20));  /* unchanged */
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "bad value") != NULL);
}

TEST(test_cmd_compliance)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "compliance 30");
    ASSERT_EQ(ctx.compliance, FP_FROM_INT(30));
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "30") != NULL);
}

TEST(test_cmd_resistance)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "resistance 8");
    ASSERT_EQ(ctx.resistance, FP_FROM_INT(8));
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "8") != NULL);
}

TEST(test_cmd_kp)
{
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    char out[TX_BUF_SIZE];
    tx_read(&ctx, out, sizeof(out));

    feed_cmd(&ctx, "kp 50");  /* 50 tenths = 5.0 */
    fp16_t expected = (fp16_t)((int64_t)50 * FP_ONE / 10);
    ASSERT_EQ(ctx.kp, expected);
    tx_read(&ctx, out, sizeof(out));
    ASSERT(strstr(out, "5.0") != NULL);
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

    printf("\nFixed-point math:\n");
    RUN(test_fp_mul_basic);
    RUN(test_fp_mul_fractional);
    RUN(test_fp_div_basic);
    RUN(test_fp_div_fractional);
    RUN(test_fp_div_by_zero_saturates);
    RUN(test_fp_negative);
    RUN(test_tx_put_fp);
    RUN(test_tx_put_fp_negative);

    printf("\nState machine constants:\n");
    RUN(test_state_names);
    RUN(test_state_durations);
    RUN(test_total_cycle_duration);
    RUN(test_default_pressure_targets);

    printf("\nRuntime configuration:\n");
    RUN(test_init_copies_default_durations);
    RUN(test_init_copies_default_pressure_targets);
    RUN(test_init_lung_defaults);
    RUN(test_init_pid_defaults);
    RUN(test_custom_duration_applied);

    printf("\nenter_state:\n");
    RUN(test_enter_state_inhale);
    RUN(test_enter_state_hold);
    RUN(test_enter_state_exhale);
    RUN(test_enter_state_inhale_resets_volume_to_peep);
    RUN(test_enter_state_exhale_resets_integrator);
    RUN(test_enter_state_hold_preserves_integrator);
    RUN(test_state_log_message);
    RUN(test_state_log_noop_when_no_change);
    RUN(test_enter_state_invalid_is_noop);

    printf("\nLung model:\n");
    RUN(test_lung_zero_duty_zero_pressure);
    RUN(test_lung_pressure_rises_with_duty);
    RUN(test_lung_exhale_pressure_decays);
    RUN(test_lung_volume_clamped_at_max);
    RUN(test_lung_peep_valve_clamp);

    printf("\nPID controller:\n");
    RUN(test_pid_positive_error_increases_duty);
    RUN(test_pid_clamp_max);
    RUN(test_pid_clamp_min);
    RUN(test_pid_anti_windup);

    printf("\nClosed-loop integration:\n");
    RUN(test_closed_loop_reaches_target);
    RUN(test_closed_loop_exhale_settles_to_peep);
    RUN(test_closed_loop_full_cycle);

    printf("\nState machine ticks:\n");
    RUN(test_tick_decrements_state_ticks);
    RUN(test_tick_no_transition_returns_zero);
    RUN(test_tick_transition_returns_one);
    RUN(test_tick_count_increments);

    printf("\nRX buffer:\n");
    RUN(test_rx_put_get);
    RUN(test_rx_get_empty_returns_zero);
    RUN(test_rx_overflow_drops);

    printf("\nCommand processing:\n");
    RUN(test_cmd_status);
    RUN(test_cmd_set_duration);
    RUN(test_cmd_duration_not_multiple_of_tick);
    RUN(test_cmd_set_target);
    RUN(test_cmd_target_bad_state);
    RUN(test_cmd_target_over_50);
    RUN(test_cmd_compliance);
    RUN(test_cmd_resistance);
    RUN(test_cmd_kp);
    RUN(test_cmd_bad_value);
    RUN(test_cmd_unknown);
    RUN(test_cmd_empty_line_ignored);
    RUN(test_cmd_overflow_value);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return 0;
}
