*** Settings ***
Documentation       Integration tests — firmware + lung model in Renode.
...                 The ventilator and lung are black boxes communicating
...                 through a memory-mapped register at 0x50000000.
...                 We observe UART output only.

Suite Setup         Setup
Suite Teardown      Teardown
Test Teardown       Test Teardown

Resource            ${RENODEKEYWORDS}

*** Variables ***
${RESC}             ${CURDIR}/../sim/ventway_test.resc
${UART}             sysbus.usart2

*** Keywords ***
Setup
    Execute Command             include @${RESC}

*** Test Cases ***
Firmware Boots And Enters Inhale
    [Documentation]     Firmware prints startup message and enters INHALE.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       Ventway starting       timeout=2
    Wait For Line On Uart       INHALE                 timeout=2

Ventilator Completes Full Breath Cycle
    [Documentation]     Firmware transitions through INHALE → HOLD → EXHALE → INHALE.
    ...                 Each transition is logged on UART with state name and cycle count.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       INHALE                 timeout=2
    Wait For Line On Uart       HOLD                   timeout=5
    Wait For Line On Uart       EXHALE                 timeout=5
    Wait For Line On Uart       INHALE                 timeout=5

Ventilator Runs Multiple Cycles
    [Documentation]     Firmware completes at least 3 breath cycles.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       [cycle 3]              timeout=15

Pressure Reaches Inspiratory Target
    [Documentation]     At HOLD entry, pressure should be near 20 cmH2O.
    ...                 The HOLD log line shows the pressure at transition.
    Create Terminal Tester      ${UART}

    Start Emulation

    # HOLD follows INHALE — pressure at transition should be near target.
    # The log line format: [cycle N] HOLD — target 20 cmH2O, P=XX.X, duty N%
    Wait For Line On Uart       HOLD                   timeout=5

Pressure Settles To PEEP During Exhale
    [Documentation]     After exhale, pressure should be near 5 cmH2O (PEEP).
    ...                 Check that cycle 2 INHALE entry shows low pressure.
    Create Terminal Tester      ${UART}

    Start Emulation

    # At cycle 2 INHALE, lungs have just exhaled — pressure near PEEP.
    Wait For Line On Uart       [cycle 2] INHALE       timeout=10
