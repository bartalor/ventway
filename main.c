/*
 * Ventway — Bare metal STM32F407 ventilator breathing cycle simulator
 *
 * State machine: INHALE -> HOLD -> EXHALE -> INHALE ...
 * TIM2 interrupt drives state transitions.
 * TIM3 CH1 (PA6) outputs PWM for simulated turbine.
 * USART2 (PA2 TX) logs state and cycle count.
 *
 * Timing at ~20 breaths/min (3s cycle):
 *   INHALE: 1.0s   — turbine at 80% duty
 *   HOLD:   0.5s   — turbine at 30% duty
 *   EXHALE: 1.5s   — turbine off (0%)
 */

#include <stdint.h>

/* ---- Register addresses ------------------------------------------------- */

/* RCC */
#define RCC_BASE        0x40023800
#define RCC_AHB1ENR     (*(volatile uint32_t *)(RCC_BASE + 0x30))
#define RCC_APB1ENR     (*(volatile uint32_t *)(RCC_BASE + 0x40))

/* GPIOA */
#define GPIOA_BASE      0x40020000
#define GPIOA_MODER     (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_AFRL      (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

/* USART2 */
#define USART2_BASE     0x40004400
#define USART2_SR       (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_DR       (*(volatile uint32_t *)(USART2_BASE + 0x04))
#define USART2_BRR      (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_CR1      (*(volatile uint32_t *)(USART2_BASE + 0x0C))

/* TIM2 — state machine timer */
#define TIM2_BASE       0x40000000
#define TIM2_CR1        (*(volatile uint32_t *)(TIM2_BASE + 0x00))
#define TIM2_DIER       (*(volatile uint32_t *)(TIM2_BASE + 0x0C))
#define TIM2_SR         (*(volatile uint32_t *)(TIM2_BASE + 0x10))
#define TIM2_PSC        (*(volatile uint32_t *)(TIM2_BASE + 0x28))
#define TIM2_ARR        (*(volatile uint32_t *)(TIM2_BASE + 0x2C))

/* TIM3 — PWM for turbine */
#define TIM3_BASE       0x40000400
#define TIM3_CR1        (*(volatile uint32_t *)(TIM3_BASE + 0x00))
#define TIM3_CCMR1      (*(volatile uint32_t *)(TIM3_BASE + 0x18))
#define TIM3_CCER       (*(volatile uint32_t *)(TIM3_BASE + 0x20))
#define TIM3_PSC        (*(volatile uint32_t *)(TIM3_BASE + 0x28))
#define TIM3_ARR        (*(volatile uint32_t *)(TIM3_BASE + 0x2C))
#define TIM3_CCR1       (*(volatile uint32_t *)(TIM3_BASE + 0x34))

/* NVIC */
#define NVIC_ISER0      (*(volatile uint32_t *)0xE000E100)

/* ---- Constants ---------------------------------------------------------- */

#define SYS_CLK         16000000U   /* HSI = 16 MHz (default after reset) */

typedef enum { INHALE, HOLD, EXHALE } state_t;

static const char *state_names[] = { "INHALE", "HOLD", "EXHALE" };

/* Duration of each state in milliseconds */
static const uint32_t state_duration_ms[] = {
    1000,   /* INHALE */
     500,   /* HOLD   */
    1500,   /* EXHALE */
};

/* PWM duty cycle per state (percent of ARR) */
static const uint32_t state_duty_pct[] = {
    80,     /* INHALE — turbine full effort */
    30,     /* HOLD   — maintain pressure   */
     0,     /* EXHALE — passive exhale       */
};

/* ---- Globals ------------------------------------------------------------ */

static volatile state_t current_state = INHALE;
static volatile uint32_t cycle_count  = 0;
static volatile uint32_t tick_count   = 0;
static volatile uint32_t state_ticks  = 0;  /* ticks remaining in state */

#define TICK_MS 10  /* TIM2 fires every 10 ms */

/* ---- UART --------------------------------------------------------------- */

static void uart_putc(char c)
{
    while (!(USART2_SR & (1 << 7)));  /* Wait for TXE */
    USART2_DR = (uint32_t)c;
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void uart_put_uint(uint32_t n)
{
    char buf[10];
    int i = 0;

    if (n == 0) {
        uart_putc('0');
        return;
    }
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i--)
        uart_putc(buf[i]);
}

/* ---- PWM ---------------------------------------------------------------- */

static void pwm_set_duty(uint32_t pct)
{
    uint32_t arr = TIM3_ARR;
    TIM3_CCR1 = (arr * pct) / 100;
}

/* ---- Peripheral init ---------------------------------------------------- */

static void clock_init(void)
{
    /* Enable GPIOA, TIM2, TIM3, USART2 clocks */
    RCC_AHB1ENR |= (1 << 0);                   /* GPIOA */
    RCC_APB1ENR |= (1 << 0) | (1 << 1) | (1 << 17);
    /*              TIM2        TIM3        USART2    */
}

static void gpio_init(void)
{
    /*
     * PA2 = USART2_TX  (AF7)
     * PA6 = TIM3_CH1   (AF2)
     * Both pins: alternate function mode (0b10)
     */

    /* PA2: AF mode */
    GPIOA_MODER &= ~(3U << (2 * 2));
    GPIOA_MODER |=  (2U << (2 * 2));
    /* PA2 AF7 (USART2) — AFRL bits [11:8] */
    GPIOA_AFRL &= ~(0xFU << (2 * 4));
    GPIOA_AFRL |=  (7U   << (2 * 4));

    /* PA6: AF mode */
    GPIOA_MODER &= ~(3U << (6 * 2));
    GPIOA_MODER |=  (2U << (6 * 2));
    /* PA6 AF2 (TIM3) — AFRL bits [27:24] */
    GPIOA_AFRL &= ~(0xFU << (6 * 4));
    GPIOA_AFRL |=  (2U   << (6 * 4));
}

static void usart2_init(void)
{
    /* 115200 baud @ 16 MHz APB1: BRR = 16000000 / 115200 ≈ 139 = 0x8B */
    USART2_BRR = SYS_CLK / 115200;
    USART2_CR1 = (1 << 13) | (1 << 3);  /* UE | TE */
}

static void tim3_pwm_init(void)
{
    /* 1 kHz PWM: PSC=15, ARR=999 -> 16MHz/16/1000 = 1kHz */
    TIM3_PSC  = 15;
    TIM3_ARR  = 999;
    TIM3_CCR1 = 0;

    /* PWM mode 1 on CH1: OC1M = 110, OC1PE = 1 */
    TIM3_CCMR1 = (6U << 4) | (1U << 3);
    /* Enable CH1 output */
    TIM3_CCER = (1U << 0);
    /* Start timer */
    TIM3_CR1 = (1U << 0);
}

static void tim2_tick_init(void)
{
    /* 10 ms tick: PSC=15999, ARR=9 -> 16MHz/16000/10 = 100 Hz */
    TIM2_PSC  = 15999;
    TIM2_ARR  = 9;
    /* Enable update interrupt */
    TIM2_DIER = (1U << 0);
    /* Enable TIM2 in NVIC (IRQ 28) */
    NVIC_ISER0 = (1U << 28);
    /* Start timer */
    TIM2_CR1 = (1U << 0);
}

/* ---- State machine ------------------------------------------------------ */

static void enter_state(state_t s)
{
    current_state = s;
    state_ticks = state_duration_ms[s] / TICK_MS;
    pwm_set_duty(state_duty_pct[s]);

    if (s == INHALE)
        cycle_count++;

    uart_puts("[cycle ");
    uart_put_uint(cycle_count);
    uart_puts("] ");
    uart_puts(state_names[s]);
    uart_puts(" — duty ");
    uart_put_uint(state_duty_pct[s]);
    uart_puts("%\r\n");
}

void TIM2_IRQHandler(void)
{
    TIM2_SR &= ~(1U << 0);  /* Clear UIF */
    tick_count++;

    if (state_ticks > 0) {
        state_ticks--;
        return;
    }

    /* Transition */
    switch (current_state) {
    case INHALE: enter_state(HOLD);   break;
    case HOLD:   enter_state(EXHALE); break;
    case EXHALE: enter_state(INHALE); break;
    }
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    clock_init();
    gpio_init();
    usart2_init();
    tim3_pwm_init();
    tim2_tick_init();

    uart_puts("Ventway starting\r\n");
    enter_state(INHALE);

    while (1) {
        /* CPU idles; state machine runs in interrupt context */
        __asm volatile ("wfi");
    }
}
