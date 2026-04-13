# ventway

Bare metal STM32F4 ventilator breathing cycle simulator, inspired by the Inovytec Ventway Sparrow turbine ventilator. Runs in Renode without physical hardware.

## What it does

Simulates the core control logic of a ventilator — a breathing cycle state machine with turbine speed control and cycle monitoring.

## States

- INHALE — turbine at high speed
- EXHALE — turbine at low speed  
- HOLD — turbine off

## How it works

A timer interrupt drives state transitions at configurable intervals. PWM output maps to turbine speed per state. UART logs current state and cycle count in real time.

## Stack

- Bare metal C, no HAL, no RTOS
- Register-level STM32F4 peripheral control
- arm-none-eabi-gcc + Makefile
- Renode for hardware simulation

## Why

The Ventway Sparrow is a turbine-based ventilator. At its core it controls airflow in precise cycles. This project models that fundamental logic at the embedded firmware level.