#include <stdint.h>

extern uint32_t _stack_top;
extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;

extern int main(void);
void Reset_Handler(void);
void Default_Handler(void);
void TIM2_IRQHandler(void)    __attribute__((weak, alias("Default_Handler")));
void USART2_IRQHandler(void)  __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector")))
const uint32_t vector_table[] = {
    (uint32_t)&_stack_top,
    (uint32_t)Reset_Handler,
    (uint32_t)Default_Handler,  /* NMI */
    (uint32_t)Default_Handler,  /* HardFault */
    (uint32_t)Default_Handler,  /* MemManage */
    (uint32_t)Default_Handler,  /* BusFault */
    (uint32_t)Default_Handler,  /* UsageFault */
    0, 0, 0, 0,                 /* Reserved */
    (uint32_t)Default_Handler,  /* SVCall */
    (uint32_t)Default_Handler,  /* DebugMon */
    0,                          /* Reserved */
    (uint32_t)Default_Handler,  /* PendSV */
    (uint32_t)Default_Handler,  /* SysTick */
    /* External interrupts: IRQ0..IRQ27 */
    0, 0, 0, 0, 0, 0, 0, 0,    /* IRQ 0-7 */
    0, 0, 0, 0, 0, 0, 0, 0,    /* IRQ 8-15 */
    0, 0, 0, 0, 0, 0, 0, 0,    /* IRQ 16-23 */
    0, 0, 0, 0,                 /* IRQ 24-27 */
    (uint32_t)TIM2_IRQHandler,  /* IRQ 28 = TIM2 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, /* IRQ 29-37 */
    (uint32_t)USART2_IRQHandler,/* IRQ 38 = USART2 */
};

void Reset_Handler(void)
{
    /* Copy .data from flash to SRAM */
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;

    /* Zero .bss */
    dst = &_sbss;
    while (dst < &_ebss)
        *dst++ = 0;

    main();
    while (1);
}

void Default_Handler(void)
{
    while (1);
}
