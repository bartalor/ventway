# Ventway Quick Reference

## What is this project
Bare metal ventilator controller on STM32F4. No OS, no HAL. Runs in Renode (hardware simulator). PID controls turbine speed to hit pressure targets. Fixed-point math, no FPU.

## Peripherals and registers
A peripheral is any hardware block on the chip that isn't the CPU — timers, UART, GPIO. The CPU talks to them through registers. They do their job independently once configured.

Every peripheral is dead until you turn on its clock (RCC). Then you wire pins to the right peripheral — PA2 to UART for serial, PA6 to timer for PWM. Timers are just counters — one counts to make 1kHz PWM for the turbine, another counts to fire the 10ms control loop. TIM2 starts last because it kicks off the interrupts and everything must be ready.

Interrupts need two enables: one on the peripheral side, one on the CPU side (NVIC). Miss either and the handler never fires.

## Control loop
Every 10ms TIM2 fires an interrupt: read pressure sensor → PID tick → update state machine → write new PWM duty. That's the whole loop.

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
Three modules, strict boundaries:
1. **ventway/** — firmware. Reads pressure from 0x50000000, writes duty to TIM3. Knows nothing about lungs.
2. **lung_model/** — patient physics. Pure C, RC circuit. Knows nothing about firmware.
3. **sim/** — glue. Renode connects the two through memory-mapped registers.

Firmware doesn't know it's simulated. Same registers as real hardware. The sim layer closes the loop by turning PWM output into pressure input.
