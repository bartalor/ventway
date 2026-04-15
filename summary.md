# Ventway Quick Reference

## What is this project
Bare metal ventilator controller on STM32F4. No OS, no HAL. Runs in Renode (hardware simulator). PID controls turbine speed to hit pressure targets. Fixed-point math, no FPU.

## Peripherals and registers
A peripheral is any hardware block on the chip that isn't the CPU — timers, UART, GPIO. The CPU talks to them through registers. They do their job independently once configured.

Every peripheral is dead until you turn on its clock (RCC — power saving, only turn on what you need). Then you wire pins to the right peripheral — GPIO alternate function config connects the physical pin to the internal peripheral. Then configure each peripheral.

Three init levels:
1. **Clocks** — RCC enables for GPIO, TIM2, TIM3, USART2
2. **Pins** — PA2 → USART2 TX (AF7), PA6 → TIM3 PWM (AF2). Mappings fixed by datasheet.
3. **Peripherals** — all same level hardware-wise, but TIM2 started last (software concern — its ISR uses everything else):
   - **USART2**: 115200 baud (16MHz / 139). Fast enough for logs within the 10ms tick, slow enough to be universal. Interrupt on RX byte → ring buffer.
   - **TIM3 (PWM)**: prescaler 15 → 1MHz tick, ARR 999 → 1kHz PWM, 1000 duty steps. Inaudible to motor, 0.1% resolution.
   - **TIM2 (tick)**: prescaler 15999 → 1kHz tick, ARR 9 → 10ms period (100Hz). ~25× oversampling of lung dynamics (RC τ=250ms).

Interrupts need two enables: one on the peripheral side, one on the CPU side (NVIC). Miss either and the handler never fires.

## TIM2 interrupt (every 10ms)
TIM2 fires → ISR clears the interrupt flag → calls `state_machine_tick()`:
1. Decrement state timer. If not expired: read pressure from sensor register (0x50000000), run PID, PID outputs duty percentage.
2. If timer expired: transition to next state (INHALE→HOLD→EXHALE→INHALE). On EXHALE entry specifically — zero the PID integral, zero duty, reset derivative memory. Other transitions don't touch PID.
3. Back in the ISR: take the duty percentage PID computed, write it to TIM3 (PWM hardware). TIM3 converts that to a real waveform on PA6 that drives the turbine motor.

That's the entire closed loop. Sensor → PID → PWM, 100 times a second.

## Main loop
After all peripherals are initialized and TIM2 is started (last, because it kicks off interrupts), main enters an infinite loop:
1. Check if state changed (flag set by ISR) → if yes, log the transition over UART (state name, pressure, duty).
2. Drain RX ring buffer — bytes arrive from UART interrupt into a ring buffer, main loop pulls them out and feeds them to the command parser (runtime PID tuning, timing changes).
3. Flush TX ring buffer to UART hardware — ISR writes log lines into a ring buffer, main loop actually pushes bytes out over serial.
4. `WFI` — wait for interrupt. CPU sleeps until next TIM2 tick or UART byte arrives. Saves power, no busy-waiting.

## State machine
INHALE (1s, target 20) → HOLD (0.5s, target 20) → EXHALE (1.5s, target 5). Repeat. ~20 breaths/min.

HOLD exists to let flow drop to zero so you see plateau pressure (pure elastic V/C) — clinically important.

On EXHALE entry: zero PID integral, zero duty, reset derivative memory. Other transitions don't touch PID state.

## PID
- P: proportional to error (target - pressure)
- I: sum of past errors — eliminates steady-state error. Summing not averaging — measures "how long off target"
- D: derivative on measurement (not error) to avoid setpoint kicks
- Anti-windup: back-calculation. When output saturates (clamped to 0-100%), the difference feeds back into the integral to pull it down. Prevents integral from growing unbounded during saturation.

## Integration testing
Three separate processes, strict boundaries:

1. **ventway** — firmware process. Runs on its own (emulated) CPU. Only talks through registers — reads pressure from `0x50000000`, writes duty to TIM3. Knows nothing about lungs.
2. **lung_model** — patient process. Independent physics simulation. Receives flow, computes pressure (RC circuit). Knows nothing about firmware.
3. **sim** — glue process. Connects the other two through the register interface. Reads duty from ventway, feeds it to lung_model as flow, takes pressure back, writes it to the sensor register.

Each process is a separate codebase, separate build, separate unit tests. They can only communicate through the register interface — no function calls, no shared memory, no imports. Process isolation enforces the boundary.

**The closed loop:** ventway writes duty → sim reads it → lung_model turns it into pressure → sim writes pressure to `0x50000000` → ventway reads it next tick. Same loop as real hardware: ventilator → turbine → air → patient → pressure sensor → ventilator.

**Startup:** Robot Framework (4th process) orchestrates:
1. Start **sim** — sets up the register interface
2. Start **lung_model** — connects to sim, ready to receive flow
3. Start **ventway** — firmware boots, starts reading registers, control loop goes live

Once all three are up, Robot just watches UART output. Pure black-box: it only sees what the firmware prints over serial, same as a real operator. Tests check state transitions happen, pressure hits targets, duty is zero during exhale.
