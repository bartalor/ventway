# ventway

Bare metal STM32F4 ventilator controller inspired by the Ventway Sparrow. Runs in Renode.

## Architecture

- **Controller** (firmware): reads pressure from sensor register (`0x50000000`), PID outputs PWM duty to turbine. States: INHALE → HOLD → EXHALE.
- **Patient** (lung model): `lung_model.c` — single C implementation. Tests link directly, Renode loads as `.so` via ctypes (`lung_model.py`).

Controller and patient communicate only through memory-mapped registers.

## Execution

10ms TIM2 ISR → `sensor_read()` → `pid_tick()` → state machine. Main loop drains UART TX. Q16.16 fixed-point, no FPU.

## Stack

Bare metal C, no HAL/RTOS, arm-none-eabi-gcc, Renode, 60 host tests.