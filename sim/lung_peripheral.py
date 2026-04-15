"""
Renode Python peripheral — Simulated lung / patient model.

Maps at 0x50000000 (defined in ventway.repl).
The firmware reads pressure from offset 0x00 (Q16.16 cmH2O).
The peripheral reads TIM3 PWM duty from the bus and runs the
lung model (loaded from lung_model.so) to close the feedback loop.

This is the Renode integration layer: it wires the two black boxes
(firmware and lung model) together through memory-mapped registers.
It uses only the lung model's public C API (lung_init, lung_tick)
via the built .so artifact.

NOTE: This script runs inside Renode's IronPython runtime.
Renode injects `self` (the peripheral instance) into the
script's global namespace — no explicit imports needed.
Pylance/mypy cannot resolve this; that is expected.
"""

# pyright: reportMissingModuleSource=false, reportUndefinedVariable=false

import ctypes
import os

# -- Load the shared library ------------------------------------------------

_script_dir = os.path.dirname(os.path.abspath(__file__))
_lib_path = os.path.join(_script_dir, "..", "build", "lung_model.so")
_lib = ctypes.CDLL(_lib_path)

# -- ctypes declarations matching lung_model.so public ABI ----------------

class LungCtx(ctypes.Structure):
    _fields_ = [
        ("volume",     ctypes.c_int32),
        ("compliance", ctypes.c_int32),
        ("resistance", ctypes.c_int32),
        ("pressure",   ctypes.c_int32),
        ("noise_seed", ctypes.c_uint32),
        ("noise_pct",  ctypes.c_int32),
    ]

_lib.lung_init.argtypes = [ctypes.POINTER(LungCtx)]
_lib.lung_init.restype  = None

_lib.lung_tick.argtypes = [ctypes.POINTER(LungCtx), ctypes.c_int32]
_lib.lung_tick.restype  = ctypes.c_int32

# -- Initialize lung state -------------------------------------------------

_lung = LungCtx()
_lib.lung_init(ctypes.byref(_lung))

# -- System parameters (ventilator side, not lung properties) --------------

FP_ONE   = 1 << 16
K_DRIVE  = FP_ONE // 2    # 0.5 cmH2O per %duty -> 50 cmH2O at 100%
PEEP     = 5 * FP_ONE     # 5 cmH2O

# -- TIM3 register addresses (for reading PWM duty) -----------------------

TIM3_ARR  = 0x4000042C
TIM3_CCR1 = 0x40000434


def tick():
    """Advance lung model by one 10ms step."""
    # Read duty cycle from TIM3 PWM
    arr  = self.Machine.SystemBus.ReadDoubleWord(TIM3_ARR)
    ccr1 = self.Machine.SystemBus.ReadDoubleWord(TIM3_CCR1)
    if arr == 0:
        duty_pct = 0
    else:
        duty_pct = (ccr1 * 100) // arr

    # Compute source pressure (ventilator side logic)
    if duty_pct == 0:
        # No drive — exhale: source is PEEP
        p_source = PEEP
    else:
        # Inhale: source = k_drive * duty
        p_source = (K_DRIVE * duty_pct) >> 16
        # Sealed airway: source never drops below current elastic recoil
        p_elastic = _lung.pressure  # approximate from last tick
        if p_source < p_elastic:
            p_source = p_elastic

    _lib.lung_tick(ctypes.byref(_lung), p_source)


def read(offset, count):
    """Firmware reads pressure from offset 0x00."""
    if offset == 0x00:
        tick()
        # Return pressure as unsigned 32-bit (Q16.16 bit pattern)
        return _lung.pressure & 0xFFFFFFFF
    self.Log(1, "lung_model: unexpected read at offset 0x{:02X}".format(offset))
    return 0


def write(offset, count, value):
    """Writes are ignored — pressure is read-only from firmware side."""
    self.Log(1, "lung_model: unexpected write at offset 0x{:02X}".format(offset))
