# Ventway

Bare metal STM32F407 ventilator breathing cycle simulator, inspired by the [Inovytec Ventway Sparrow](https://www.inovytec.com/ventway-sparrow/).

No HAL — pure register-level C.

## How it maps to a real ventilator

A mechanical ventilator delivers air in a repeating **Inhale → Hold → Exhale** cycle.
The Ventway Sparrow uses **Pressure Control Ventilation (PCV)** — the turbine targets
a constant airway pressure, and a PID controller adjusts motor speed in real time.

| Phase   | Duration | Pressure target | What happens                              |
|---------|----------|-----------------|-------------------------------------------|
| INHALE  | 1.0 s    | 20 cmH₂O       | PID drives turbine to reach inspiratory pressure. Flow is highest at start (empty lungs, large pressure gap) and decelerates as lungs fill — the classic PCV waveform. |
| HOLD    | 0.5 s    | 20 cmH₂O       | PID holds plateau pressure for gas exchange. Turbine effort is minimal — just compensating for leaks. |
| EXHALE  | 1.5 s    | 5 cmH₂O (PEEP) | Passive exhalation driven by lung elastic recoil. PEEP valve maintains positive end-expiratory pressure to prevent alveolar collapse. |

This gives a 3-second cycle = **20 breaths per minute**, a typical adult setting.

### Closed-loop control

Real ventilators don't use precalculated duty curves — they use closed-loop feedback.
This simulator models that with:

1. **Simulated lung** — a first-order single-compartment model with tunable compliance
   (mL/cmH₂O) and airway resistance (cmH₂O/(L/s)). Turbine duty maps to airflow,
   airflow integrates into volume, and volume/compliance gives elastic pressure.

2. **PID controller** — reads simulated airway pressure, compares to the per-state
   target, and outputs a duty percentage to the PWM. Includes anti-windup
   (back-calculation) and derivative-on-measurement to avoid setpoint kicks.

3. **PEEP valve model** — clamps lung volume so pressure never drops below the
   exhale target, matching a real mechanical PEEP valve.

The decelerating inspiratory flow pattern, plateau hold, and passive PEEP-limited
exhale all **emerge naturally** from the controller chasing pressure targets into
the lung model — no hand-tuned waveforms.

### Physiological defaults

| Parameter            | Default | Typical clinical range | Unit          |
|----------------------|---------|------------------------|---------------|
| Lung compliance      | 50      | 30–80                  | mL/cmH₂O     |
| Airway resistance    | 5       | 5–20                   | cmH₂O/(L/s)  |
| Inspiratory pressure | 20      | 10–35                  | cmH₂O        |
| PEEP                 | 5       | 3–10                   | cmH₂O        |
| Turbine flow gain    | 10      | —                      | (mL/s) / %   |

### Integer math

All arithmetic uses Q16.16 fixed-point (no FPU). The Cortex-M4 `SMULL` instruction
handles the 32×32→64 multiply; division happens once per tick at 100 Hz.

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
[cycle 1] INHALE — target 20 cmH2O, P=0.0, duty 100%
[cycle 1] HOLD — target 20 cmH2O, P=19.8, duty 12%
[cycle 1] EXHALE — target 5 cmH2O, P=20.1, duty 0%
[cycle 2] INHALE — target 20 cmH2O, P=5.2, duty 85%
...
```

### UART commands

All parameters are tunable at runtime over UART (115200 baud, send with `\r` or `\n`):

| Command                     | Example              | Effect                                |
|-----------------------------|----------------------|---------------------------------------|
| `status`                    | `status`             | Print all current parameters          |
| `<state> <ms>`              | `inhale 1200`        | Set state duration (must be multiple of 10ms) |
| `target <state> <cmH2O>`   | `target inhale 25`   | Set pressure target                   |
| `compliance <value>`        | `compliance 30`      | Set lung compliance (mL/cmH₂O)       |
| `resistance <value>`        | `resistance 8`       | Set airway resistance (cmH₂O/(L/s))  |
| `kp <tenths>`               | `kp 30`              | Set Kp = 3.0 (value ÷ 10)            |
| `ki <tenths>`               | `ki 10`              | Set Ki = 1.0 (value ÷ 10)            |
| `kd <tenths>`               | `kd 1`               | Set Kd = 0.1 (value ÷ 10)            |

## Project structure

```
ventway.h    — types, context struct, fixed-point macros, API declarations
ventway.c    — state machine, lung model, PID controller, ring buffers, commands
main.c       — peripheral init, ISRs, main loop
startup.c    — vector table, Reset_Handler, .data/.bss init
linker.ld    — STM32F407VG memory layout (1MB flash, 128KB SRAM)
ventway.resc — Renode emulation script
Makefile     — build with arm-none-eabi-gcc
```
