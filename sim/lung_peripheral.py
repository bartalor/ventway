# -*- coding: utf-8 -*-
#
# Renode PythonPeripheral -- glue between firmware and lung_model.so.
#
# Maps at 0x50000000 (defined in build/ventway.repl).
# Offset 0x00: pressure readback (Q16.16 cmH2O)
# Offset 0x04: duty % written by bus watchpoint hook (see .resc)
#
# Lung physics lives in lung_model.so (single source of truth).
# This script handles only:
#   - Renode peripheral protocol (request.IsInit/IsRead/IsWrite)
#   - Turbine model: duty % -> source pressure (sim-specific)
#   - Calling lung_tick() with the source pressure

# pyright: reportMissingModuleSource=false, reportUndefinedVariable=false

import ctypes

# -- Q16.16 constants (must match lung_model.h) --

FP_SHIFT = 16
FP_ONE   = 1 << FP_SHIFT

# -- Turbine model (sim-specific, not part of lung) --

K_DRIVE = FP_ONE // 2    # 0.5 cmH2O per %duty -> 50 cmH2O at 100%
PEEP    = 5 * FP_ONE     # 5 cmH2O baseline when duty=0

# -- lung_ctx_t layout (must match lung_model.h) --

class LungCtx(ctypes.Structure):
    _fields_ = [
        ("volume",     ctypes.c_int32),
        ("compliance", ctypes.c_int32),
        ("resistance", ctypes.c_int32),
        ("pressure",   ctypes.c_int32),
        ("noise_seed", ctypes.c_uint32),
        ("noise_pct",  ctypes.c_int32),
    ]

# -- Initialization --

if request.IsInit:
    so = ctypes.CDLL("__LUNG_SO__")
    so.lung_init.restype = None
    so.lung_tick.restype = ctypes.c_int32

    lung = LungCtx()
    so.lung_init(ctypes.byref(lung))
    duty_pct = 0

# -- Read: firmware reads pressure --

if request.IsRead:
    if request.Offset == 0x00:
        # Turbine model: convert duty to source pressure
        if duty_pct == 0:
            p_source = PEEP
        else:
            p_source = K_DRIVE * duty_pct
            if p_source < lung.pressure:
                p_source = lung.pressure

        # Delegate physics to lung_model.so
        so.lung_tick(ctypes.byref(lung), ctypes.c_int32(p_source))
        request.Value = lung.pressure & 0xFFFFFFFF
    else:
        request.Value = 0

# -- Write: duty from bus watchpoint hook --

if request.IsWrite:
    if request.Offset == 0x04:
        duty_pct = int(request.Value)
