# GDB Python diagnostic: trace what happens during EXHALE.
#
# Goal: figure out why lung pressure stays at 20 cmH2O during EXHALE
# instead of decaying toward PEEP (~5 cmH2O).
#
# Three hypotheses to differentiate:
#   1. Watchpoint mirror skips duty=0 writes (.resc line: `if value == 0: return`)
#      → lung peripheral never sees duty=0, keeps pushing.
#   2. Firmware doesn't write 0 to TIM3_CCR1 during EXHALE.
#   3. Lung model doesn't decay even when duty=0.
#
# The script breaks on the EXHALE transition, samples TIM3_CCR1 and the
# lung registers, then runs forward N ticks to see whether pressure decays.

import gdb

TIM3_CCR1   = 0x40000434
LUNG_PRESS  = 0x50000000
LUNG_DUTY   = 0x50000004

INHALE, HOLD, EXHALE = 0, 1, 2

SAMPLE_TICKS = 30  # ~300 ms simulated, plenty for exhale to start decaying


def read_u32(addr):
    return int(gdb.parse_and_eval(f"*(unsigned int *) {addr:#x}"))


def fp16_to_cmh2o(raw):
    # Q16.16 — pressure stored as cmH2O << 16
    return raw / 65536.0


def banner(msg):
    print()
    print("=" * 72)
    print(msg)
    print("=" * 72)


def connect_and_setup():
    gdb.execute("set pagination off")
    gdb.execute("set confirm off")
    gdb.execute("target remote :3333")


def run_until_exhale_entry():
    """Break at state_machine_tick when state just transitioned to EXHALE.

    state_changed is set by enter_state(), so condition fires on the first
    tick of the new state.
    """
    gdb.execute("break state_machine_tick")
    bp_num = gdb.breakpoints()[-1].number
    gdb.execute(f"condition {bp_num} ctx->state == {EXHALE} && ctx->state_changed")

    print("Running firmware until EXHALE entry...")
    gdb.execute("continue")

    # Cleanup so subsequent `continue` calls don't re-stop here
    gdb.execute(f"delete {bp_num}")


def sample_state(label):
    ctx = gdb.parse_and_eval("g_ctx")
    state = int(ctx["state"])
    state_name = ["INHALE", "HOLD", "EXHALE"][state]
    duty_pct = int(ctx["duty_pct"])
    pressure_q16 = int(ctx["pressure"])
    cycle = int(ctx["cycle_count"])
    tim3_ccr1 = read_u32(TIM3_CCR1)
    lung_press_raw = read_u32(LUNG_PRESS)
    lung_duty = read_u32(LUNG_DUTY)

    print(f"[{label}]")
    print(f"  cycle={cycle} state={state_name}({state})")
    print(f"  firmware:  duty_pct={duty_pct}  pressure={fp16_to_cmh2o(pressure_q16):.2f} cmH2O")
    print(f"  TIM3_CCR1: {tim3_ccr1}  (firmware-written PWM compare; ARR=999)")
    print(f"  lung reg:  duty={lung_duty}  pressure={fp16_to_cmh2o(lung_press_raw):.2f} cmH2O")
    print()


def step_n_ticks(n):
    """Run forward n state_machine_tick invocations."""
    gdb.execute("break state_machine_tick")
    bp_num = gdb.breakpoints()[-1].number

    for i in range(n):
        gdb.execute("continue")
    gdb.execute(f"delete {bp_num}")


def diagnose():
    if read_u32(LUNG_DUTY) == 0:
        print("Hypothesis 1 (watchpoint drops duty=0): REJECTED")
        print("  Lung saw duty=0 — the mirror does forward zeros.")
    else:
        print("Hypothesis 1 (watchpoint drops duty=0): SUPPORTED")
        print(f"  Lung still has duty={read_u32(LUNG_DUTY)} after EXHALE entry.")

    if read_u32(TIM3_CCR1) == 0:
        print("Hypothesis 2 (firmware doesn't write 0): REJECTED")
        print("  TIM3_CCR1 == 0 — firmware did write 0.")
    else:
        print("Hypothesis 2 (firmware doesn't write 0): SUPPORTED")
        print(f"  TIM3_CCR1 == {read_u32(TIM3_CCR1)} — firmware kept driving.")


def main():
    connect_and_setup()

    banner("EXHALE entry diagnostic")
    run_until_exhale_entry()
    sample_state("at EXHALE entry")

    print(f"Stepping {SAMPLE_TICKS} ticks (~{SAMPLE_TICKS*10} ms simulated)...")
    step_n_ticks(SAMPLE_TICKS)
    sample_state(f"after {SAMPLE_TICKS} ticks")

    banner("Diagnosis")
    diagnose()

    gdb.execute("detach")
    gdb.execute("quit")


main()
