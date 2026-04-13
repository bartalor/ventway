# Ventway

Bare metal STM32F407 ventilator breathing cycle simulator, inspired by the [Inovytec Ventway Sparrow](https://www.inovytec.com/ventway-sparrow/).

No HAL — pure register-level C.

## How it maps to a real ventilator

A mechanical ventilator delivers air in a repeating **Inhale → Hold → Exhale** cycle:

| Phase   | Duration | What happens (real device)                | Simulation                        |
|---------|----------|-------------------------------------------|-----------------------------------|
| INHALE  | 1.0 s    | Turbine spins up, pushes air into patient | PWM at 80% duty                   |
| HOLD    | 0.5 s    | Turbine holds pressure for gas exchange   | PWM at 30% duty                   |
| EXHALE  | 1.5 s    | Valve opens, patient exhales passively    | PWM at 0% (turbine off)           |

This gives a 3-second cycle = **20 breaths per minute**, a typical adult setting.

The PWM output on TIM3 CH1 (PA6) represents the turbine motor drive signal. In a real device this would feed a BLDC motor controller.

## Peripherals used

| Peripheral | Purpose                                  |
|------------|------------------------------------------|
| TIM2       | 10 ms tick interrupt — drives state machine |
| TIM3 CH1   | 1 kHz PWM output — simulated turbine motor  |
| USART2     | 115200 baud TX — state/cycle logging         |
| GPIOA      | Alternate function pins (PA2, PA6)           |

## Building

```
make
```

Requires `arm-none-eabi-gcc`. Produces `ventway.elf` and `ventway.bin`.

## Running in Renode

```
make renode
```

Or manually:

```
renode ventway.resc
```

UART2 output appears in the Renode analyzer window:

```
Ventway starting
[cycle 1] INHALE — duty 80%
[cycle 1] HOLD — duty 30%
[cycle 1] EXHALE — duty 0%
[cycle 2] INHALE — duty 80%
...
```

## Project structure

```
main.c       — state machine, peripheral init, ISR, UART logging
startup.c    — vector table, Reset_Handler, .data/.bss init
linker.ld    — STM32F407VG memory layout (1MB flash, 128KB SRAM)
ventway.resc — Renode emulation script
Makefile     — build with arm-none-eabi-gcc
```
