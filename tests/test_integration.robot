*** Settings ***
Documentation       Integration tests — firmware + lung model in Renode.
...                 The ventilator and lung are black boxes communicating
...                 through a memory-mapped register at 0x50000000.
...                 We observe UART output only.
...
...                 UART log format:
...                 [cycle N] STATE — target X cmH2O, P=Y.Z, duty N%

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
    [Documentation]     Firmware prints startup banner and first state transition.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       Ventway starting       timeout=2
    Wait For Line On Uart       INHALE                 timeout=2

Ventilator Completes Full Breath Cycle
    [Documentation]     Firmware transitions INHALE → HOLD → EXHALE → INHALE.
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

Pressure Near Target At End Of Inhale
    [Documentation]     At HOLD entry, PID has driven pressure near 20 cmH2O.
    ...                 Accept 15–25 cmH2O (±5 of target).
    Create Terminal Tester      ${UART}

    Start Emulation

    # HOLD log line contains the pressure at transition from INHALE
    Wait For Line On Uart       regex:\\[cycle 1\\] HOLD.*P=(1[5-9]|2[0-5])\\.\\d    timeout=5

Pressure Near PEEP After Exhale
    [Documentation]     At cycle-2 INHALE entry, lungs have just exhaled.
    ...                 Pressure should be near PEEP (5 cmH2O): accept 2–9 cmH2O.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       regex:\\[cycle 2\\] INHALE.*P=[2-9]\\.\\d             timeout=10

Pressure Consistent Across Cycles
    [Documentation]     HOLD pressure stays near target across multiple cycles.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       regex:\\[cycle 1\\] HOLD.*P=(1[5-9]|2[0-5])\\.\\d     timeout=5
    Wait For Line On Uart       regex:\\[cycle 2\\] HOLD.*P=(1[5-9]|2[0-5])\\.\\d     timeout=10
    Wait For Line On Uart       regex:\\[cycle 3\\] HOLD.*P=(1[5-9]|2[0-5])\\.\\d     timeout=15

Duty Zero During Exhale
    [Documentation]     Exhale is passive — duty must be 0%.
    Create Terminal Tester      ${UART}

    Start Emulation

    Wait For Line On Uart       regex:\\[cycle 1\\] EXHALE.*duty 0%                   timeout=5
