/*
 * Ventway — Bare metal STM32F407 ventilator breathing cycle simulator
 *
 * State machine: INHALE -> HOLD -> EXHALE -> INHALE ...
 * TIM2 interrupt drives 10ms tick — PID + lung model per tick.
 * TIM3 CH1 (PA6) outputs PWM for simulated turbine.
 * USART2 (PA2 TX/RX) logs state and accepts runtime commands.
 *
 * Closed-loop PCV at ~20 breaths/min (3s cycle):
 *   INHALE: 1.0s   — PID targets 20 cmH2O inspiratory pressure
 *   HOLD:   0.5s   — PID holds 20 cmH2O plateau
 *   EXHALE: 1.5s   — passive exhale, PEEP valve at 5 cmH2O
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
#define NVIC_ISER1      (*(volatile uint32_t *)0xE000E104)

/* ---- Constants ---------------------------------------------------------- */

/* RCC bit positions */
#define RCC_AHB1ENR_GPIOAEN    (1U << 0)
#define RCC_APB1ENR_TIM2EN     (1U << 0)
#define RCC_APB1ENR_TIM3EN     (1U << 1)
#define RCC_APB1ENR_USART2EN   (1U << 17)

/* GPIO pin numbers */
#define PA2                    2
#define PA6                    6

/* GPIO modes (MODER register) */
#define GPIO_MODE_AF           2U

/* Alternate function mappings */
#define GPIO_AF7_USART2        7U
#define GPIO_AF2_TIM3          2U

/* ---- Global context ----------------------------------------------------- */

static volatile ventway_ctx_t g_ctx;

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
    /* PA2: AF mode for USART2 TX */
    GPIOA_MODER &= ~(3U << (PA2 * 2));
    GPIOA_MODER |=  (GPIO_MODE_AF << (PA2 * 2));
    GPIOA_AFRL  &= ~(0xFU << (PA2 * 4));
    GPIOA_AFRL  |=  (GPIO_AF7_USART2 << (PA2 * 4));

    /* PA6: AF mode for TIM3 PWM */
    GPIOA_MODER &= ~(3U << (PA6 * 2));
    GPIOA_MODER |=  (GPIO_MODE_AF << (PA6 * 2));
    GPIOA_AFRL  &= ~(0xFU << (PA6 * 4));
    GPIOA_AFRL  |=  (GPIO_AF2_TIM3 << (PA6 * 4));
}

static void usart2_init(void)
{
    USART2_BRR = SYS_CLK / 115200;
    USART2_CR1 = (1U << 13) | (1U << 3) | (1U << 2) | (1U << 5);  /* UE | TE | RE | RXNEIE */
    NVIC_ISER1 = (1U << (38 - 32));  /* Enable USART2 IRQ (IRQ38) */
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
static void uart_flush(volatile ventway_ctx_t *ctx)
{
    while (ctx->tx_tail != ctx->tx_head) {
        while (!(USART2_SR & (1U << 7)));  /* Wait for TXE */
        USART2_DR = (uint32_t)ctx->tx_buf[ctx->tx_tail];
        ctx->tx_tail = (ctx->tx_tail + 1) & (TX_BUF_SIZE - 1);
    }
}

/* ---- ISRs --------------------------------------------------------------- */

void TIM2_IRQHandler(void)
{
    TIM2_SR &= ~(1U << 0);  /* Clear UIF */
    state_machine_tick((ventway_ctx_t *)&g_ctx);
    pwm_set_duty(g_ctx.duty_pct);
}

void USART2_IRQHandler(void)
{
    if (USART2_SR & (1U << 5)) {  /* RXNE */
        char c = (char)(USART2_DR & 0xFF);
        rx_put((ventway_ctx_t *)&g_ctx, c);
    }
}

/* ---- Main --------------------------------------------------------------- */

int main(void)
{
    clock_init();
    gpio_init();
    usart2_init();
    tim3_pwm_init();

    ventway_init((ventway_ctx_t *)&g_ctx);
    tx_puts((ventway_ctx_t *)&g_ctx, "Ventway starting\r\n");
    enter_state((ventway_ctx_t *)&g_ctx, INHALE);
    state_log((ventway_ctx_t *)&g_ctx);
    pwm_set_duty(g_ctx.duty_pct);

    tim2_tick_init();

    while (1) {
        /* Log state transitions (flag set by ISR) */
        state_log((ventway_ctx_t *)&g_ctx);

        /* Process incoming commands */
        char c;
        while (rx_get((ventway_ctx_t *)&g_ctx, &c))
            cmd_process_byte((ventway_ctx_t *)&g_ctx, c);

        uart_flush(&g_ctx);
        __asm volatile ("wfi");
    }
}
