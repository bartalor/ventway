/*
 * test_integration.c — Integration tests: lung + ventilator as separate processes
 *
 * Mirrors the real system: ventilator and lung run in separate processes,
 * communicating through shared memory (like the memory-mapped register
 * at 0x50000000 in Renode).  Each process has its own independent 10ms
 * timer (timerfd) — no synchronization between them.
 *
 * Build:  make test-integration
 * Run:    ./build/test_integration
 */

#define _GNU_SOURCE  /* timerfd */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ventway.h"
#include "lung_model.h"

/* ---- Shared memory: the register on the bus ----------------------------- */

typedef struct {
    volatile fp16_t   pressure;    /* lung → vent (sensor register) */
    volatile uint32_t duty_pct;    /* vent → lung (PWM duty) */
    volatile uint32_t is_exhale;   /* vent → lung (breath phase) */
} bus_regs_t;

typedef struct {
    fp16_t   pressure;
    uint32_t state;
    uint32_t cycle_count;
} test_result_t;

static bus_regs_t *bus_alloc(void)
{
    bus_regs_t *bus = mmap(NULL, sizeof(bus_regs_t),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (bus == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    memset((void *)bus, 0, sizeof(*bus));
    return bus;
}

static void bus_free(bus_regs_t *bus)
{
    munmap(bus, sizeof(*bus));
}

/* ---- 10ms timer --------------------------------------------------------- */

static int timer_create_10ms(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (fd < 0) {
        perror("timerfd_create");
        exit(1);
    }
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 0, .tv_nsec = 10000000 },  /* 10ms */
        .it_value    = { .tv_sec = 0, .tv_nsec = 10000000 },
    };
    if (timerfd_settime(fd, 0, &ts, NULL) < 0) {
        perror("timerfd_settime");
        exit(1);
    }
    return fd;
}

static void timer_wait(int fd)
{
    uint64_t exp;
    if (read(fd, &exp, sizeof(exp)) != sizeof(exp)) {
        perror("timerfd read");
        exit(1);
    }
}

/* ---- Drive logic (ventilator side) -------------------------------------- */

#define K_DRIVE     (FP_ONE / 2)    /* 0.5 cmH2O per %duty */
#define PEEP_CMHO   FP_FROM_INT(5)  /* 5 cmH2O */

static fp16_t drive_p_source(uint32_t duty_pct, int is_exhale, fp16_t p_lung)
{
    if (is_exhale)
        return PEEP_CMHO;
    fp16_t p_source = fp_mul(K_DRIVE, FP_FROM_INT((int32_t)duty_pct));
    if (p_source < p_lung)
        p_source = p_lung;  /* sealed airway */
    return p_source;
}

/* ---- Lung process (child) ----------------------------------------------- */

static void lung_process(bus_regs_t *bus, int n_ticks,
                         uint32_t seed, int noise_pct)
{
    lung_ctx_t lung;
    lung_init(&lung);
    if (seed)
        lung_set_noise(&lung, seed, noise_pct);

    bus->pressure = lung.pressure;

    int tfd = timer_create_10ms();

    for (int i = 0; i < n_ticks; i++) {
        timer_wait(tfd);

        /* Read duty from bus, compute source pressure, tick physics */
        fp16_t p_src = drive_p_source(bus->duty_pct, bus->is_exhale,
                                      lung.pressure);
        lung_tick(&lung, p_src);

        /* Write pressure to bus */
        bus->pressure = lung.pressure;
    }

    close(tfd);
    _exit(0);
}

/* ---- Vent process (parent) ---------------------------------------------- */

static void vent_process(ventway_ctx_t *ctx, bus_regs_t *bus, int n_ticks)
{
    int tfd = timer_create_10ms();

    for (int i = 0; i < n_ticks; i++) {
        timer_wait(tfd);

        /* sensor_read (reads bus->pressure via sensor_reg) + PID + transitions */
        state_machine_tick(ctx);

        /* Write duty + phase to bus */
        bus->duty_pct  = ctx->duty_pct;
        bus->is_exhale = (ctx->state == EXHALE) ? 1 : 0;
    }

    close(tfd);
}

/* ---- Test harness ------------------------------------------------------- */

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

/* ---- Helper: run vent + lung as two processes --------------------------- */

typedef struct {
    int      n_ticks;
    uint32_t noise_seed;
    int      noise_pct;
} test_params_t;

static test_result_t run_test(const test_params_t *p)
{
    bus_regs_t *bus = bus_alloc();

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        /* Child: lung process — own timer, own address space */
        lung_process(bus, p->n_ticks, p->noise_seed, p->noise_pct);
        /* never returns */
    }

    /* Parent: vent process — own timer, own address space */
    ventway_ctx_t ctx;
    ventway_init(&ctx);
    ctx.sensor_reg = (volatile fp16_t *)&bus->pressure;
    enter_state(&ctx, INHALE);

    vent_process(&ctx, bus, p->n_ticks);

    /* Wait for lung to finish */
    int status;
    waitpid(pid, &status, 0);

    test_result_t result;
    result.pressure    = ctx.pressure;
    result.state       = ctx.state;
    result.cycle_count = ctx.cycle_count;

    bus_free(bus);
    return result;
}

/* ---- Tests -------------------------------------------------------------- */

TEST(test_vent_reaches_target)
{
    /* 80 ticks: stays in INHALE (100 tick phase), PID settles by ~40 */
    test_params_t p = { .n_ticks = 80 };
    test_result_t r = run_test(&p);
    ASSERT_FP_NEAR(r.pressure, 20, 5);
}

TEST(test_vent_exhale_settles_to_peep)
{
    /* Run long enough for inhale + hold + exhale to settle */
    test_params_t p = { .n_ticks = 500 };
    test_result_t r = run_test(&p);
    ASSERT_FP_NEAR(r.pressure, 5, 3);
}

TEST(test_vent_full_cycle)
{
    /* INHALE=100 + HOLD=50 + EXHALE=150 = 300 ticks per cycle.
     * Run 2 full cycles + extra */
    test_params_t p = { .n_ticks = 610 };
    test_result_t r = run_test(&p);
    ASSERT(r.cycle_count >= 3);
}

TEST(test_vent_noisy_lung)
{
    /* 80 ticks: stays in INHALE, PID settles despite noise */
    test_params_t p = { .n_ticks = 80, .noise_seed = 42, .noise_pct = 10 };
    test_result_t r = run_test(&p);
    ASSERT_FP_NEAR(r.pressure, 20, 5);
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    printf("Running integration tests (two-process)...\n\n");

    RUN(test_vent_reaches_target);
    RUN(test_vent_exhale_settles_to_peep);
    RUN(test_vent_full_cycle);
    RUN(test_vent_noisy_lung);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return 0;
}
