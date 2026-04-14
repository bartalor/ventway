# ventway

Bare metal STM32F4 ventilator breathing cycle simulator, inspired by the Inovytec Ventway Sparrow turbine ventilator. Runs in Renode without physical hardware.

## What it does

Simulates the core control logic of a ventilator — closed-loop Pressure Control Ventilation (PCV) with a simulated lung model and PID controller.

## States

- INHALE — PID drives turbine to reach inspiratory pressure (20 cmH2O)
- HOLD — PID holds plateau pressure for gas exchange (20 cmH2O)
- EXHALE — passive exhalation, PEEP valve maintains 5 cmH2O

## How it works

A 10ms timer tick drives a PID controller that reads simulated airway pressure and outputs turbine duty. A first-order lung model (tunable compliance + resistance) converts duty to pressure. The decelerating inspiratory flow pattern emerges naturally from the controller chasing a constant pressure target into a filling lung.

All math is Q16.16 fixed-point — no FPU, no floating point anywhere.

## Stack

- Bare metal C, no HAL, no RTOS
- Register-level STM32F4 peripheral control
- arm-none-eabi-gcc + Makefile
- Renode for hardware simulation

## Why

The Ventway Sparrow is a turbine-based ventilator. At its core it controls airflow in precise cycles. This project models that fundamental logic at the embedded firmware level.