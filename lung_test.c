/*
 * lung_test.c — Unit tests for the lung model (pure physics, no ventilator)
 *
 * Build:  make test-lung
 * Run:    ./build/lung_test
 */

#include <stdio.h>
#include <stdlib.h>

#include "lung_model.h"

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

/* ---- Tests -------------------------------------------------------------- */

TEST(test_lung_at_rest)
{
    lung_ctx_t lung;
    lung_init(&lung);

    fp16_t initial_pressure = lung.pressure;

    /* No ventilator: p_source = 0 for 100 ticks */
    for (int i = 0; i < 100; i++)
        lung_tick(&lung, 0);

    /* Lung deflates to zero pressure */
    ASSERT_FP_NEAR(lung.pressure, 0, 1);
    /* Started above zero */
    ASSERT(initial_pressure > 0);
}

TEST(test_lung_inflates_with_pressure)
{
    lung_ctx_t lung;
    lung_init(&lung);

    fp16_t vol_before = lung.volume;

    /* Apply 20 cmH2O for 50 ticks */
    for (int i = 0; i < 50; i++)
        lung_tick(&lung, FP_FROM_INT(20));

    /* Volume should increase */
    ASSERT(lung.volume > vol_before);
    /* Pressure should approach 20 */
    ASSERT_FP_NEAR(lung.pressure, 20, 3);
}

TEST(test_lung_deflates_from_pressure_removal)
{
    lung_ctx_t lung;
    lung_init(&lung);

    /* Inflate */
    for (int i = 0; i < 100; i++)
        lung_tick(&lung, FP_FROM_INT(20));

    fp16_t vol_inflated = lung.volume;

    /* Remove pressure */
    for (int i = 0; i < 200; i++)
        lung_tick(&lung, 0);

    /* Volume should decrease */
    ASSERT(lung.volume < vol_inflated);
}

TEST(test_lung_reaches_equilibrium)
{
    lung_ctx_t lung;
    lung_init(&lung);

    /* Apply constant 15 cmH2O for a long time */
    for (int i = 0; i < 500; i++)
        lung_tick(&lung, FP_FROM_INT(15));

    /* At equilibrium: flow ≈ 0, pressure ≈ p_source */
    ASSERT_FP_NEAR(lung.pressure, 15, 1);
}

TEST(test_lung_noise_perturbs_response)
{
    lung_ctx_t clean, noisy;
    lung_init(&clean);
    lung_init(&noisy);
    lung_set_noise(&noisy, 123, 10);

    fp16_t p_src = FP_FROM_INT(25);
    int differs = 0;
    for (int i = 0; i < 50; i++) {
        fp16_t p_clean = lung_tick(&clean, p_src);
        fp16_t p_noisy = lung_tick(&noisy, p_src);
        if (p_clean != p_noisy)
            differs = 1;
    }
    ASSERT(differs);
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    printf("Running lung model tests...\n\n");

    RUN(test_lung_at_rest);
    RUN(test_lung_inflates_with_pressure);
    RUN(test_lung_deflates_from_pressure_removal);
    RUN(test_lung_reaches_equilibrium);
    RUN(test_lung_noise_perturbs_response);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return 0;
}
