# Register Configuration — Requirements to Values

System clock: 16 MHz (STM32F4 default HSI).

## RCC — Clock Enable

Must power on every peripheral before touching its registers. On STM32, peripherals are off by default to save power — you enable each one's clock individually.

| Register | Bit | Peripheral | Why |
|---|---|---|---|
| AHB1ENR | GPIOAEN | GPIO port A | PA2 is USART2 TX (serial output), PA6 is TIM3 CH1 (PWM to turbine). These pin-to-peripheral mappings are fixed by the STM32F4 — not a choice, the datasheet dictates which pins connect to which peripherals via alternate functions. |
| APB1ENR | TIM2EN | Timer 2 | 10ms system tick. Why 10ms: 100 Hz is fast enough for closed-loop pressure control (lung pressure changes on the order of hundreds of ms), slow enough to not waste CPU on a Cortex-M4. |
| APB1ENR | TIM3EN | Timer 3 | PWM output to turbine motor. Why TIM3: its channel 1 routes to PA6 (hardware-fixed), and TIM2 is already taken by the tick timer. TIM3 is the simplest available timer with a PWM-capable channel on a free pin. |
| APB1ENR | USART2EN | UART 2 | Serial for logging and commands. Why USART2: it's the UART whose TX line is on PA2, which is broken out on common STM32F4 dev boards (e.g. Nucleo routes PA2 to the ST-Link virtual COM port). |

AHB1 is the high-speed bus (GPIO). APB1 is the lower-speed bus (timers, UART). Which bus a peripheral sits on is fixed by the chip — not a choice.

## GPIO — Pin Assignment

Pins are general-purpose by default. We reassign them to their peripheral functions.

Each pin needs two settings:
1. **MODER** — switch from general-purpose to alternate function mode
2. **AFRL** — pick which alternate function number (which peripheral gets the pin)

| Pin | Mode | AF | Peripheral | Why this pin |
|---|---|---|---|---|
| PA2 | Alternate function | AF7 | USART2 TX | Hardware-fixed: the STM32F4 datasheet maps USART2_TX to PA2 via AF7. No other AF number gives USART2 on this pin. |
| PA6 | Alternate function | AF2 | TIM3 CH1 | Hardware-fixed: TIM3_CH1 is available on PA6 via AF2. This is the PWM output that drives the turbine motor. |

Why not other PA pins? We only configure pins we actually use. PA2 and PA6 are dictated by the peripheral routing — if we needed USART2 TX, PA2 (or PD5) are the only options. If we needed TIM3 CH1, PA6 (or PB4/PC6) are the only options.

## USART2 — Serial Port

**Requirement:** 115200 baud serial for logging state transitions and receiving runtime commands.

Why 115200: standard high-speed baud rate, fast enough to print state logs every 10ms without bottlenecking. Higher rates (e.g. 921600) would work but aren't needed.

| Register | Value | What it does |
|---|---|---|
| BRR | 16 MHz / 115200 = 139 | Baud rate divider — the hardware uses this to time each bit. Derived directly from system clock and desired baud rate. |
| CR1 | UE | Turn on the UART hardware |
| CR1 | TE | Enable transmitting — we need to send log output |
| CR1 | RE | Enable receiving — we accept runtime commands (e.g. change PID gains) |
| CR1 | RXNEIE | Fire interrupt when a byte arrives. Without this we'd have to poll, wasting CPU cycles in the main loop. |
| NVIC | IRQ 38 | Tell the CPU to deliver USART2 interrupts to USART2_IRQHandler. Without this, the UART raises the interrupt but the CPU ignores it. IRQ number 38 for USART2 is fixed by the STM32F4 interrupt table. |

## TIM3 — PWM for Turbine

**Requirement:** generate a variable-duty PWM signal on PA6 to control turbine speed.

Why ~1 kHz PWM frequency: fast enough that the motor sees a smooth average voltage (no audible whine), slow enough that the timer has good duty resolution (1000 steps from 0% to 100%).

| Register | Value | How it's derived |
|---|---|---|
| PSC | 15 | Prescaler divides system clock: 16 MHz / (15+1) = 1 MHz timer clock. Why 15: gives a round 1 MHz, making the math simple. |
| ARR | 999 | Auto-reload: timer counts 0→999 then resets. 1000 counts at 1 MHz = 1 kHz PWM. Also gives exactly 1000 duty steps (0.1% resolution). |
| CCR1 | 0 | Capture/compare for channel 1. Starts at 0 = turbine off. The PID controller writes this every 10ms to adjust turbine speed. |
| CCMR1 | PWM mode 1 + preload | Output is high while counter < CCR1, low when counter >= CCR1. Preload means CCR1 updates take effect at the next cycle boundary (no glitches mid-cycle). |
| CCER | Output enable | Actually connect the timer's internal output to the physical pin PA6. Without this, the timer runs but the pin doesn't toggle. |
| CR1 | Enable | Start the counter. |

**Duty control:** `pwm_set_duty(pct)` writes `CCR1 = ARR * pct / 100`. So duty 50% → CCR1 = 499 → high for 500/1000 of the cycle.

## TIM2 — 10ms System Tick

**Requirement:** periodic interrupt every 10ms — the heartbeat of the entire system. Every tick: read pressure sensor, run PID, update state machine.

Why 10ms (100 Hz): lung pressure changes over hundreds of milliseconds (breath cycle is 3 seconds). 100 Hz gives ~100 samples per breath — plenty for the PID to track pressure smoothly. Faster (1ms) would waste CPU. Slower (100ms) would make the PID sluggish and pressure overshoot.

| Register | Value | How it's derived |
|---|---|---|
| PSC | 15999 | Prescaler: 16 MHz / (15999+1) = 1 kHz timer clock. Different from TIM3's prescaler because we need a slower tick here, not a fast PWM. |
| ARR | 9 | Count 0→9 (10 counts at 1 kHz = 10ms period). Why not PSC=15, ARR=9999 for the same result? Either works — this way the prescaler does the heavy division and ARR stays small. |
| DIER | UIE | Update Interrupt Enable — fire interrupt when counter overflows (hits ARR and resets). |
| NVIC | IRQ 28 | Tell CPU to deliver TIM2 interrupts to TIM2_IRQHandler. IRQ 28 is fixed by the STM32F4 interrupt table. |
| CR1 | Enable | Start counting. **This is the last init call** — once TIM2 starts, interrupts fire every 10ms and the control loop is live. Everything before this was setup; this flips the switch. |
