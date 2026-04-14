"""
Renode Python peripheral — Simulated lung / patient model.

Maps at 0x50000000 (defined in ventway.repl).
The firmware reads pressure from offset 0x00 (Q16.16 cmH2O).
The peripheral reads TIM3 PWM duty from the bus and runs a
single-compartment lung model to close the feedback loop.

This is the "patient" — entirely outside the firmware.

NOTE: This script runs inside Renode's IronPython runtime.
Renode injects `self` (the peripheral instance) into the
script's global namespace — no explicit imports needed.
Pylance/mypy cannot resolve this; that is expected.
"""

# pyright: reportMissingModuleSource=false, reportUndefinedVariable=false

FP_SHIFT = 16
FP_ONE   = 1 << FP_SHIFT

# -- Lung parameters (matching previous firmware defaults) --
COMPLIANCE = 50 * FP_ONE      # 50 mL/cmH2O
RESISTANCE = 5 * FP_ONE       # 5 cmH2O/(L/s)
K_TURB     = 10 * FP_ONE      # 10 (mL/s) per %duty
PEEP       = 5 * FP_ONE       # 5 cmH2O
DT_FP      = FP_ONE // 100    # 10 ms in Q16.16

MAX_VOLUME = 1000 * FP_ONE    # 1000 mL safety clamp

# -- TIM3 register addresses (for reading PWM duty) --
TIM3_ARR   = 0x4000042C
TIM3_CCR1  = 0x40000434

# -- State --
lung_volume = COMPLIANCE * PEEP // FP_ONE   # start at C * PEEP
pressure_fp = PEEP                           # start at PEEP

def fp_mul(a, b):
    return (a * b) >> FP_SHIFT

def fp_div(a, b):
    if b == 0:
        return 0x7FFFFFFF if a >= 0 else -0x80000000
    return (a << FP_SHIFT) // b


def tick():
    """Advance lung model by one 10ms step. Called by Renode timer."""
    global lung_volume, pressure_fp

    # Read duty cycle from TIM3 PWM
    arr  = self.Machine.SystemBus.ReadDoubleWord(TIM3_ARR)
    ccr1 = self.Machine.SystemBus.ReadDoubleWord(TIM3_CCR1)
    if arr == 0:
        duty_pct = 0
    else:
        duty_pct = (ccr1 * 100) // arr

    # Determine flow based on whether turbine is active
    # We infer exhale from duty == 0 (controller sets duty to 0 on exhale)
    if duty_pct == 0:
        # Passive exhale: flow driven by pressure above PEEP
        p_elastic = fp_div(lung_volume, COMPLIANCE)
        p_drive = p_elastic - PEEP
        if p_drive < 0:
            p_drive = 0
        flow = -(fp_div(p_drive, RESISTANCE)) * 1000
    else:
        # Active: turbine drives flow = k_turb * duty
        flow = fp_mul(K_TURB, duty_pct * FP_ONE)

    # Update volume
    lung_volume += fp_mul(flow, DT_FP)

    # PEEP valve clamp
    min_vol = fp_mul(COMPLIANCE, PEEP)
    if lung_volume < min_vol:
        lung_volume = min_vol

    # Safety clamp
    if lung_volume > MAX_VOLUME:
        lung_volume = MAX_VOLUME

    # Compute airway pressure: P = V/C + R*flow/1000
    p_elastic   = fp_div(lung_volume, COMPLIANCE)
    p_resistive = fp_mul(RESISTANCE, flow) // 1000
    pressure_fp = p_elastic + p_resistive
    if pressure_fp < 0:
        pressure_fp = 0


def read(offset, count):
    """Firmware reads pressure from offset 0x00."""
    if offset == 0x00:
        tick()
        # Return pressure as 32-bit signed value (Q16.16)
        return pressure_fp & 0xFFFFFFFF
    return 0


def write(offset, count, value):
    """Writes are ignored — pressure is read-only from firmware side."""
    pass
