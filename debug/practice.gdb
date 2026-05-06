# GDB practice script for ventway running in Renode.
#
# Setup:
#   1. `make renode` — starts Renode and the firmware.
#   2. In Renode's monitor: `machine StartGdbServer 3333`
#   3. In another terminal, from repo root:
#         gdb-multiarch -x debug/practice.gdb build/ventway.elf
#
# Each section below is an independent exercise — comment/uncomment to try one
# at a time. Don't run them all at once.

target remote :3333

# Show the firmware's entry point and current PC.
info registers pc
list main

# ---------------------------------------------------------------------------
# Exercise 1: software breakpoint on a function
# ---------------------------------------------------------------------------
# break vent_tick
# commands
#   silent
#   printf "tick: state=%d cycle=%d P=%d duty=%d\n", state, cycle, last_pressure, last_duty
#   continue
# end
# continue

# ---------------------------------------------------------------------------
# Exercise 2: hardware breakpoint
# ---------------------------------------------------------------------------
# Why `hb` over `b`? Software breakpoints rewrite the instruction at the BP
# address with a trap. That works in RAM but fails in Flash (read-only on real
# hardware). On Cortex-M4 there are typically 6 hardware breakpoint slots.
#
# hb vent_tick
# continue

# ---------------------------------------------------------------------------
# Exercise 3: conditional breakpoint
# ---------------------------------------------------------------------------
# Stop only when entering EXHALE. State enum values live in ventway.h —
# adjust the literal if it differs.
#
# break ventway.c:vent_tick if state == STATE_EXHALE
# continue

# ---------------------------------------------------------------------------
# Exercise 4: watchpoint on a variable
# ---------------------------------------------------------------------------
# Break whenever `state` is written. Hardware watchpoints on Cortex-M4 are
# limited (4 DWT comparators) — exhausting them gives "Could not insert
# hardware watchpoint" at runtime.
#
# watch state
# continue

# ---------------------------------------------------------------------------
# Exercise 5: examine a memory-mapped peripheral register
# ---------------------------------------------------------------------------
# TIM3_CCR1 holds the PWM compare value the firmware writes for duty cycle.
# 0x40000434 = TIM3 base (0x40000400) + CCR1 offset (0x34).
#
# x/wx 0x40000434
#
# Lung peripheral at 0x50000000:
#   0x00 = pressure (Q16.16, cmH2O)
#   0x04 = duty (0-100, written by the watchpoint mirror)
# x/2wx 0x50000000

# ---------------------------------------------------------------------------
# Exercise 6: backtrace from inside an ISR
# ---------------------------------------------------------------------------
# break TIM2_IRQHandler
# continue
# bt
# info registers
# # Stacked frame on Cortex-M exception entry: R0-R3, R12, LR, PC, xPSR
# # are pushed to the stack the CPU was using when the IRQ fired. `bt` may
# # not unwind across the exception boundary cleanly without -mtpcs-frame
# # build flags; that's a real-world gotcha.

# ---------------------------------------------------------------------------
# Exercise 7: poke state to force a transition
# ---------------------------------------------------------------------------
# Set state to EXHALE manually and continue. Useful for forcing rare paths.
#
# set var state = STATE_EXHALE
# continue

# ---------------------------------------------------------------------------
# Exercise 8: instruction-level stepping
# ---------------------------------------------------------------------------
# break vent_tick
# continue
# # `si` steps one instruction; `ni` steps over `bl` (branch with link / call).
# # Use `disassemble` to see the surrounding instructions.
# disassemble
# si
# si
# info registers
