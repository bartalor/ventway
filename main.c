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

#include "ventway.h"

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

/* RCC bit positions */
#define RCC_AHB1ENR_GPIOAEN    (1U << 0)
#define RCC_APB1ENR_TIM2EN     (1U << 0)
#define RCC_APB1ENR_TIM3EN     (1U << 1)
#define RCC_APB1ENR_USART2EN   (1U << 17)

/* ---- PWM ---------------------------------------------------------------- */

static void pwm_set_duty(uint32_t pct)
{
    uint32_t arr = TIM3_ARR;
    TIM3_CCR1 = (arr * pct) / 100;
}

/* ---- Peripheral init ---------------------------------------------------- */

static void clock_init(void)
{
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC_APB1ENR |= RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM3EN | RCC_APB1ENR_USART2EN;
}

static void gpio_init(void)
{
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
    USART2_BRR = SYS_CLK / 115200;
    USART2_CR1 = (1 << 13) | (1 << 3);  /* UE | TE */
}

static void tim3_pwm_init(void)
{
    TIM3_PSC  = 15;
    TIM3_ARR  = 999;
    TIM3_CCR1 = 0;
    TIM3_CCMR1 = (6U << 4) | (1U << 3);
    TIM3_CCER = (1U << 0);
    TIM3_CR1 = (1U << 0);
}

static void tim2_tick_init(void)
{
    TIM2_PSC  = 15999;
    TIM2_ARR  = 9;
    TIM2_DIER = (1U << 0);
    NVIC_ISER0 = (1U << 28);
    TIM2_CR1 = (1U << 0);
}

/* Drain ring buffer to UART — call from main loop only */
static void uart_flush(void)
{
    while (tx_tail != tx_head) {
        while (!(USART2_SR & (1U << 7)));  /* Wait for TXE */
        USART2_DR = (uint32_t)tx_buf[tx_tail];
        tx_tail = (tx_tail + 1) & (TX_BUF_SIZE - 1);
    }
}

/* ---- ISR ---------------------------------------------------------------- */

void TIM2_IRQHandler(void)
{
    TIM2_SR &= ~(1U << 0);  /* Clear UIF */
    if (state_machine_tick())
        pwm_set_duty(pwm_duty_pct);
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    clock_init();
    gpio_init();
    usart2_init();
    tim3_pwm_init();
    tim2_tick_init();

    tx_puts("Ventway starting\r\n");
    enter_state(INHALE);
    pwm_set_duty(pwm_duty_pct);

    while (1) {
        uart_flush();
        __asm volatile ("wfi");
    }
}
