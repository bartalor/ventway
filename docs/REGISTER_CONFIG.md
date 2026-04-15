# Register Configuration — Requirements to Values

System clock: 16 MHz (STM32F4 default HSI — the internal oscillator that runs on power-up without any external crystal).

Every register value below exists because of a specific ventilator requirement. Nothing is arbitrary.

---

## RCC — Clock Enable

On STM32, every peripheral is powered off by default to save energy. Before you can write to any peripheral's registers, you must enable its clock through the RCC (Reset and Clock Control) block. If you skip this step, writes to that peripheral's registers are silently ignored — the hardware isn't listening.

| Register | Bit | Peripheral | Why we need it | What happens without it |
|---|---|---|---|---|
| AHB1ENR | GPIOAEN (bit 0) | GPIO port A | Two of our peripherals need physical pins on port A: USART2 needs PA2 for serial TX (to send pressure/state logs to the operator), and TIM3 needs PA6 for PWM output (to drive the turbine motor that pushes air into the patient). Both pin assignments are fixed by the STM32F4 datasheet — PA2 is where USART2_TX is routed, PA6 is where TIM3_CH1 is routed. We can't choose different pins. | GPIO port A registers are dead. Pin configuration (MODER, AFRL) writes are ignored. PA2 and PA6 stay in their default reset state (floating input). No serial output, no PWM signal — the ventilator can't communicate or drive the turbine. |
| APB1ENR | TIM2EN (bit 0) | Timer 2 | TIM2 is the system heartbeat — it fires an interrupt every 10 ms that drives the entire control loop: read pressure sensor → run PID → update state machine → set PWM duty. Without this timer, nothing happens. Why TIM2 specifically: it's a general-purpose 32-bit timer on APB1, and we don't need it for anything else (TIM3 is taken by PWM). Any free timer would work, but TIM2 is the simplest available choice. | No 10 ms tick. The ISR never fires. Pressure is never read, PID never runs, state machine never advances. The turbine stays at whatever duty was set during init (0%). The patient gets no ventilation. |
| APB1ENR | TIM3EN (bit 1) | Timer 3 | TIM3 generates the PWM signal that controls turbine speed. The PID controller outputs a duty percentage every 10 ms, and TIM3 converts that into a hardware PWM waveform on PA6. Why TIM3 and not another timer: TIM3's channel 1 is routed to PA6 (hardware-fixed by the datasheet). TIM2 is already used for the tick. TIM3 is the next simplest timer with a PWM-capable channel on a free pin. | No PWM output. PA6 stays low. The turbine motor gets no drive signal. Even though the PID computes a duty value, there's no hardware to turn it into a real signal. Zero airflow to the patient. |
| APB1ENR | USART2EN (bit 17) | UART 2 | USART2 is the serial port — it sends state transition logs ("INHALE P=20.1 duty=45%") to the operator and receives runtime commands (like PID gain adjustments). Why USART2: its TX pin is PA2, which on common STM32F4 dev boards (like Nucleo) is routed to the ST-Link virtual COM port — so you get serial output over USB with no extra wiring. USART1 or USART3 would require different pins that aren't as conveniently broken out. | No serial communication. The ventilator runs blind — no logs, no command interface. The operator can't see pressure readings or state transitions, and can't adjust parameters at runtime. The control loop still works (PID and PWM are independent of UART), but you have no visibility into what's happening. |

**Bus layout (not a choice — fixed by the chip):**
- **AHB1** (Advanced High-performance Bus): GPIO lives here. It's the fast bus because GPIO toggling needs to be quick (e.g., bit-banging protocols). In our case, we only use GPIO for alternate function routing, not speed-critical toggling.
- **APB1** (Advanced Peripheral Bus 1): Timers and UART live here. It's the slower bus (max 42 MHz on STM32F407, though we run at 16 MHz so it doesn't matter). The chip designer placed lower-speed peripherals here to keep the fast bus uncongested.

---

## GPIO — Pin Assignment

Pins start in their default reset state (floating input, no alternate function). To connect a pin to a peripheral, you must configure two things:

1. **MODER** — switch the pin from general-purpose to alternate function mode (mode = 2). This tells the GPIO hardware: "don't treat this as a regular input/output, hand it off to a peripheral."
2. **AFRL** — select which alternate function number (0–15) the pin should use. Each pin can connect to up to 16 different peripherals depending on the AF number. The mapping is fixed by the datasheet.

| Pin | MODER value | AFRL value | Peripheral it connects to | Why this pin, this AF |
|---|---|---|---|---|
| PA2 | AF mode (2) | AF7 | USART2 TX | The STM32F4 datasheet maps USART2_TX to PA2 via AF7. This is not a design choice — if you want USART2 TX, your options are PA2 or PD5. We use PA2 because it's on the Nucleo board's ST-Link virtual COM port (free USB-serial). PD5 would need an external USB-serial adapter. No other AF number on PA2 gives USART2. AF7 is fixed. |
| PA6 | AF mode (2) | AF2 | TIM3 CH1 (PWM output) | The STM32F4 datasheet maps TIM3_CH1 to PA6 via AF2. Alternatives are PB4 or PC6, but PA6 is on port A which we already have clocked for USART2 — no need to enable another GPIO port's clock. AF2 is the only AF number that gives TIM3 on PA6. |

**What about PA3 (USART2 RX)?** We receive serial commands (PID gain adjustments), so we need RX too. However, `gpio_init()` doesn't configure PA3. This works in Renode (the simulator routes UART data regardless of GPIO config) but would be a bug on real hardware — PA3 would stay as a floating input instead of being connected to USART2's receiver. On real hardware, you'd add PA3 as AF7 just like PA2.

**Why only two pins?** We only configure pins we actually use. The ventilator needs exactly two peripheral outputs: serial TX (to send logs) and PWM (to drive the turbine). Every other pin stays in its default state. Configuring unused pins wastes init time and makes the code harder to audit.

---

## USART2 — Serial Port

**Requirement:** The ventilator needs a serial link to (1) log every state transition with pressure and duty values so the operator can monitor breathing, and (2) accept runtime commands to adjust PID gains without reflashing.

**Baud rate choice: 115200.** The system logs a line every state transition (every 0.5–1.5 seconds) plus potentially every 10 ms tick if verbose mode is on. At 115200 baud, one character takes ~87 µs to transmit. A typical log line ("INHALE P=20.1 duty=45%\r\n" = ~27 chars) takes ~2.3 ms — well within the 10 ms tick window. Higher rates like 921600 would work but aren't needed and are less universally supported by serial terminals. Lower rates like 9600 would take ~28 ms per line — longer than the tick period, causing the TX buffer to overflow and drop logs.

| Register | Value | What it does | Why this value |
|---|---|---|---|
| BRR | 139 | Baud rate divider. The UART hardware divides the peripheral clock (16 MHz) by this value to time each bit. | 16,000,000 / 115,200 = 138.89, which the hardware rounds to 139. This gives an actual baud rate of 16,000,000 / 139 = 115,108 — a 0.08% error from the target, well within the ±2% tolerance that UART receivers accept. If this value were wrong (say, 16 for 1 Mbaud), the receiving terminal would see garbage characters. |
| CR1 bit 13 | UE (USART Enable) | Powers on the UART's internal logic — the baud rate generator, shift registers, and interrupt logic all start running. | On STM32, enabling the peripheral clock (RCC) just supplies power to the register block. The peripheral itself stays dormant until you set its enable bit. Think of RCC as plugging in the device, and UE as flipping the power switch. Without UE, the BRR/TE/RE settings are written to registers but the UART hardware doesn't act on them. No bits are transmitted or received. |
| CR1 bit 3 | TE (Transmitter Enable) | Activates the transmit path. The UART can now shift data out of the data register onto the TX pin (PA2). | We need to send log output. Without TE, writing to the data register has no effect — the TX pin stays idle. The ventilator runs but you can't see any output. |
| CR1 bit 2 | RE (Receiver Enable) | Activates the receive path. The UART can now sample incoming bits on the RX pin and assemble them into bytes. | We accept runtime commands (e.g., "kp 3.5" to change the proportional gain). Without RE, incoming bytes on the RX pin are ignored — the operator can see logs but can't send commands. The ventilator still works but loses its runtime tunability. |
| CR1 bit 5 | RXNEIE (RX Not Empty Interrupt Enable) | Fires an interrupt to the CPU every time a complete byte arrives in the receive data register. | Without this, we'd have to poll the UART status register in the main loop to check if a byte arrived. Polling wastes CPU cycles (checking every WFI wakeup even when nothing arrived) and risks missing bytes if the main loop is slow. With the interrupt, the CPU wakes up only when a byte actually arrives, and `USART2_IRQHandler` immediately stashes it in the RX ring buffer. At 115200 baud, bytes arrive at most every ~87 µs — the ISR takes <1 µs, so there's no risk of overrun. |
| NVIC ISER1 | bit 6 (IRQ 38) | Tells the CPU's interrupt controller to actually deliver USART2 interrupts to the `USART2_IRQHandler` function. | The STM32F4 interrupt table assigns IRQ number 38 to USART2 — this is fixed by the chip design, not a choice. ISER1 covers IRQs 32–63, so IRQ 38 is bit (38-32) = bit 6 in ISER1. Without this NVIC enable, the UART raises the interrupt flag internally, but the CPU never sees it — `USART2_IRQHandler` never runs, and incoming bytes are lost. You need BOTH the peripheral-level enable (RXNEIE) AND the NVIC enable — either one alone is not enough. |

**Why not DMA?** DMA (Direct Memory Access) can transfer UART data without CPU involvement, which is useful for high-throughput or low-latency systems. Our log lines are short and infrequent (a few dozen bytes every 0.5–1.5 seconds). The ring buffer + ISR approach is simpler, uses no DMA channels, and the CPU overhead is negligible. DMA would add complexity (channel config, transfer complete callbacks, circular buffer management) for no measurable benefit.

---

## TIM3 — PWM for Turbine Motor

**Requirement:** Generate a variable-duty-cycle PWM signal on PA6 to control the turbine motor that pushes air into the patient. The PID controller computes a duty percentage (0–100%) every 10 ms, and TIM3 turns that into a hardware waveform.

**PWM frequency choice: ~1 kHz.** The turbine motor is an inductive load — it integrates the PWM pulses into a smooth average current. At 1 kHz, the motor sees essentially DC (no audible whine, no vibration). Lower frequencies (e.g., 100 Hz) could cause audible buzzing and jerky motor response. Higher frequencies (e.g., 20 kHz) work fine electrically but reduce duty resolution: at 20 kHz with a 16 MHz clock, you'd only get 800 steps (16M / 20k) instead of 1000, and you'd need a different prescaler. 1 kHz hits the sweet spot: inaudible to the motor, round numbers for the math, and 1000 discrete duty steps (0.1% resolution — more than enough for smooth pressure control).

| Register | Value | What it does | Why this value |
|---|---|---|---|
| PSC | 15 | Prescaler — divides the input clock before it reaches the counter. Timer clock = 16 MHz / (PSC+1) = 16 MHz / 16 = **1 MHz**. | We want the counter to tick at 1 MHz so that with ARR=999, we get exactly 1 kHz PWM. Why 15 and not some other prescaler? Because 16 MHz / 16 = 1 MHz is a clean division. A prescaler of 0 would give 16 MHz and require ARR=15999 for 1 kHz — works but wastes counter range. The "+1" in the formula is an STM32 hardware convention: PSC=0 means divide-by-1, PSC=15 means divide-by-16. |
| ARR | 999 | Auto-reload value — the counter counts from 0 up to 999, then resets to 0 and starts over. One full count = 1000 ticks at 1 MHz = **1 ms period = 1 kHz PWM**. | This also defines the duty resolution: CCR1 can be 0–999, giving exactly 1000 steps from 0% to 100%. The PID outputs whole-percent duty (0–100), and `pwm_set_duty(pct)` computes `CCR1 = ARR * pct / 100`. So duty 50% → CCR1 = 499 → output is high for 500/1000 of the cycle. With 1000 steps, each step is 0.1% — far finer than the PID's 1% output resolution, so we never lose precision in the conversion. |
| CCR1 | 0 | Capture/Compare Register for channel 1 — sets the duty cycle. When the counter is below CCR1, the output is high; when at or above, it's low. | Starts at 0 = turbine off. This is the safe startup state — the turbine doesn't spin until the PID starts producing positive duty values during the first INHALE phase. The PID writes a new CCR1 value every 10 ms via `pwm_set_duty()`. During EXHALE, the firmware sets duty to 0% (CCR1=0), letting the patient exhale passively against the PEEP valve. |
| CCMR1 | bits [6:4]=110, bit 3=1 | **PWM mode 1** (110): output is high while counter < CCR1, low when counter ≥ CCR1. **Preload** (bit 3): CCR1 updates take effect at the next counter overflow, not mid-cycle. | PWM mode 1 is the standard PWM mode — higher CCR1 = higher duty = faster turbine = more air pressure. Preload prevents glitches: if the PID writes a new CCR1 while the counter is mid-cycle, the change waits until the cycle boundary. Without preload, you could get a single truncated or extended pulse, causing a momentary voltage spike or dip to the motor. At 1 kHz the glitch would be brief, but preload costs nothing and eliminates it. |
| CCER | bit 0 (CC1E) | Output Enable — connects TIM3's internal PWM signal to the physical pin PA6. | Without this, the timer runs internally (counter counts, CCR1 compares) but the result never reaches the pin. PA6 stays low. The motor gets no signal. This is a safety feature of the STM32 design: you can configure the entire timer before connecting it to the outside world, avoiding spurious pulses during setup. We enable it last (after PSC, ARR, CCR1, and CCMR1 are set). |
| CR1 | bit 0 (CEN) | Counter Enable — starts the timer counting. | Until this bit is set, the counter is frozen at 0. No PWM cycles occur. We set this after all other TIM3 registers are configured, so the first PWM cycle is clean. Once enabled, TIM3 runs continuously and autonomously — the CPU only needs to update CCR1 when the duty changes. |

**Duty control flow:** Every 10 ms, the TIM2 ISR calls `state_machine_tick()` which calls `pid_tick()`. The PID outputs `duty_pct` (0–100). Then `pwm_set_duty(duty_pct)` computes `TIM3_CCR1 = 999 * duty_pct / 100` and writes it. The TIM3 hardware does the rest — no CPU involvement until the next tick.

---

## TIM2 — 10 ms System Tick

**Requirement:** A periodic interrupt every 10 ms — the heartbeat of the entire ventilator. Every tick: read pressure from sensor, run PID controller, advance state machine, update PWM duty. This is the only thing that makes the ventilator breathe.

**Tick rate choice: 100 Hz (10 ms).** The breath cycle is 3 seconds (INHALE 1.0s → HOLD 0.5s → EXHALE 1.5s). At 100 Hz, you get 100 samples during INHALE, 50 during HOLD, 150 during EXHALE — plenty for the PID to track pressure smoothly. The lung's pressure changes over hundreds of milliseconds (the RC time constant is R×C = 5 × 50 = 250 ms), so 10 ms sampling captures the dynamics with ~25× oversampling. Faster (1 ms) would waste 10× more CPU on ISR overhead for negligible control improvement. Slower (100 ms) would give only 10 samples during INHALE — the PID would react sluggishly, causing pressure overshoot (dangerous: too much pressure damages lung tissue) and undershoot (ineffective ventilation).

| Register | Value | What it does | Why this value |
|---|---|---|---|
| PSC | 15999 | Prescaler — divides system clock: 16 MHz / (15999+1) = 16 MHz / 16000 = **1 kHz timer clock** (one tick per millisecond). | Different from TIM3's prescaler (15) because we need a much slower interrupt rate here. TIM3 needs 1 MHz to get fine PWM resolution. TIM2 needs 1 kHz because we want a 10 ms period and want to keep ARR small. Why 15999 specifically: because 16 MHz / 16000 = 1 kHz exactly. The "+1" convention is the same as TIM3. |
| ARR | 9 | Counter counts 0→9 (10 ticks at 1 kHz = **10 ms period**). When the counter hits 9 and overflows back to 0, it generates an update event. | Why not PSC=15, ARR=9999 for the same 10 ms? Both give 16 MHz / 16 / 10000 = 100 Hz. Either works mathematically. We chose the large-prescaler approach (PSC=15999, ARR=9) so the prescaler does the heavy division and ARR stays small. This is a stylistic choice — no functional difference. ARR=9 also makes it immediately obvious that the timer counts 10 ticks per interrupt, matching the 10 ms period. |
| DIER | bit 0 (UIE) | Update Interrupt Enable — tells TIM2 to raise an interrupt flag every time the counter overflows (hits ARR and resets to 0). | This is the peripheral-side interrupt enable. It tells the timer: "when you overflow, assert your interrupt line to the NVIC." Without UIE, the counter runs but no interrupt is generated — the overflow event is silently ignored. The PID never runs. |
| NVIC ISER0 | bit 28 (IRQ 28) | Tells the CPU's interrupt controller to deliver TIM2 interrupts to `TIM2_IRQHandler`. | IRQ 28 for TIM2 is fixed by the STM32F4 interrupt vector table — not a choice. ISER0 covers IRQs 0–31, so IRQ 28 is bit 28 in ISER0. Just like USART2, you need BOTH enables: the peripheral-side (UIE in DIER) and the CPU-side (NVIC ISER). Without NVIC, TIM2 raises the flag but the CPU ignores it — `TIM2_IRQHandler` never executes. |
| CR1 | bit 0 (CEN) | Counter Enable — starts TIM2 counting. **This is the last init call in the entire startup sequence.** | Once this bit is set, interrupts fire every 10 ms and the control loop is live. Everything before this — clock enables, GPIO, USART, TIM3, ventway_init, enter_state(INHALE) — was setup. This is the "flip the switch" moment. We start TIM2 last deliberately: if we started it before TIM3 was configured, the first PID output would try to write a duty to an unconfigured PWM. If we started it before USART2, the first state log would write to a dead UART. The init order matters, and TIM2 goes last because it kicks everything into motion. |

---

## Init Order Summary

The init sequence in `main()` is not arbitrary — each step depends on the previous:

```
clock_init()       — Power on GPIO, TIM2, TIM3, USART2 (must be first — nothing works without clocks)
gpio_init()        — Route PA2→USART2, PA6→TIM3 (needs GPIO clock from step 1)
usart2_init()      — Configure baud rate, enable TX/RX/interrupt (needs GPIO routing from step 2)
tim3_pwm_init()    — Configure 1kHz PWM, start timer (needs GPIO routing from step 2)
ventway_init()     — Zero out controller state, set PID gains (no hardware dependency, but must happen before TIM2 starts)
enter_state(INHALE)— Set initial breath state and target pressure (must happen before TIM2 starts ticking)
tim2_tick_init()   — START the 10ms tick (must be LAST — everything above must be ready before interrupts fire)
```

After `tim2_tick_init()`, the system is fully autonomous: the ISR reads pressure, runs PID, updates duty, advances states — all without main loop involvement. The main loop just drains UART output and processes incoming commands.
