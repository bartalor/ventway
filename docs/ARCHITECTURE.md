# Ventway Architecture

Register init details are in [REGISTER_CONFIG.md](REGISTER_CONFIG.md).

## 1. Program Flow

```
main()
  clock_init()  ->  gpio_init()  ->  usart2_init()  ->  tim3_pwm_init()
  ventway_init()          — zero out all state, set PID gains (Kp=3, Ki=1, Kd=0.1)
  sensor_reg = &PSENS_DR  — point sensor at the memory-mapped lung peripheral
  enter_state(INHALE)     — start first breath cycle
  tim2_tick_init()        — GO: interrupts start firing

  while(1):               — main loop (non-ISR context)
    state_log()           — if state changed, print to UART
    cmd_process_byte()    — parse incoming serial commands
    uart_flush()          — drain TX ring buffer to hardware
    WFI                   — sleep until next interrupt
```

Every 10ms (TIM2 ISR):
```
TIM2_IRQHandler:
  clear interrupt flag
  state_machine_tick(&g_ctx):
    if ticks remaining:  sensor_read() -> pid_tick() -> return
    if ticks expired:    enter_state(next) -> set state_changed flag
  pwm_set_duty(g_ctx.duty_pct)  — write CCR1 to update turbine speed
```

State cycle (3s total, ~20 breaths/min):
```
INHALE (1.0s) -> HOLD (0.5s) -> EXHALE (1.5s) -> INHALE ...
```

---

## 2. Race Condition Avoidance

The ISR and main loop share `g_ctx`. Three techniques prevent races:

1. **Single-writer per field.** `duty_pct`, `pressure`, `pid_*` — only the ISR writes these. `tx_tail`, `cmd_*` — only the main loop writes these. No field has two writers.

2. **Ring buffers with separate head/tail.** TX buffer: ISR writes `tx_head`, main loop reads `tx_tail`. RX buffer: ISR writes `rx_head`, main loop reads `rx_tail`. Each index is only written by one side — no lock needed.

3. **`state_changed` flag.** ISR sets it to 1, main loop clears it to 0. One-way signal. Even if the main loop misses one, the next log just shows the newer state. No data lost, no lock needed.

4. **`volatile` on `g_ctx`.** Tells the compiler: don't cache this in registers, always read from memory. Without it, the main loop's `while(1)` could optimize away the re-reads of `tx_tail` or `state_changed`.

---

## 3. Integration Testing Architecture

### Three modules, strict boundaries

| Module | What it is | Knows about |
|---|---|---|
| **ventway/** | Firmware (controller) | Reads pressure from `0x50000000`, writes PWM duty to TIM3_CCR1. Doesn't know lungs exist. |
| **lung_model/** | Patient physics (RC circuit) | `lung_tick(ctx, p_source) -> pressure`. Pure math. Doesn't know firmware exists. |
| **sim/** | Glue layer (Renode scripts + peripheral) | Connects the two: captures CCR1 writes, converts to source pressure, calls lung_tick, writes result back to sensor register. |

### How it runs — one process, closed loop

Renode is a single process. It runs the ARM binary (ventway.elf) instruction-by-instruction and simulates all peripherals.

```
Firmware writes TIM3_CCR1 = 450
  -> Bus watchpoint hook fires, converts to duty % (45%)
  -> Writes duty to lung peripheral at 0x50000004

Firmware reads pressure from 0x50000000
  -> lung_peripheral.py fires on read
  -> Converts duty -> source pressure (turbine model)
  -> Calls lung_tick(ctx, p_source) via ctypes -> lung_model.so
  -> Returns lung.pressure to firmware
  -> PID computes new duty
  -> Loop continues
```

The firmware doesn't know it's in a simulator. It just reads a register and writes a register — exactly like real hardware. The sim layer closes the loop by turning PWM output into pressure input.

### `make stats` — two comparisons
1. **Baseline** (`lung_baseline.c`): runs lung model alone with p_source=0 for 8 cycles. Shows what happens to the patient with no ventilator.
2. **With ventilator** (Renode): runs the full closed-loop sim for 30s, logs UART, then `parse_stats.py` computes pressure error vs targets.
