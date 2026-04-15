#!/usr/bin/env python3
"""
Parse UART log from Renode and compute pressure error statistics.

Usage: python3 sim/parse_stats.py build/uart_log.txt

UART log format:
  [cycle N] STATE — target X cmH2O, P=Y.Z, duty N%
"""

import re
import sys
import math


def parse_log(path):
    """Parse UART log lines into a list of (cycle, state, target, pressure, duty) tuples."""
    pattern = re.compile(
        r"\[cycle (\d+)\] (\w+) .* target (\d+) cmH2O, P=(-?\d+\.\d+), duty (\d+)%"
    )
    entries = []
    with open(path) as f:
        for line in f:
            m = pattern.search(line)
            if m:
                entries.append((
                    int(m.group(1)),
                    m.group(2),
                    float(m.group(3)),
                    float(m.group(4)),
                    int(m.group(5)),
                ))
    return entries


def compute_stats(entries):
    """Compute per-state error statistics."""
    by_state = {}
    for cycle, state, target, pressure, duty in entries:
        by_state.setdefault(state, []).append((target, pressure, duty, cycle))

    stats = {}
    for state, samples in by_state.items():
        errors = [pressure - target for target, pressure, duty, cycle in samples]
        abs_errors = [abs(e) for e in errors]
        n = len(errors)

        stats[state] = {
            "n": n,
            "mean_error": sum(errors) / n,
            "mean_abs_error": sum(abs_errors) / n,
            "max_abs_error": max(abs_errors),
            "rms_error": math.sqrt(sum(e * e for e in errors) / n),
            "min_pressure": min(p for _, p, _, _ in samples),
            "max_pressure": max(p for _, p, _, _ in samples),
            "mean_duty": sum(d for _, _, d, _ in samples) / n,
        }
    return stats


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 sim/parse_stats.py <uart_log.txt>")
        sys.exit(1)

    entries = parse_log(sys.argv[1])
    if not entries:
        print("No log entries found. Check log format.")
        sys.exit(1)

    print(f"Parsed {len(entries)} state transitions "
          f"({entries[-1][0]} cycles)\n")

    stats = compute_stats(entries)

    # Print in state-machine order
    state_order = ["INHALE", "HOLD", "EXHALE"]
    for state in state_order:
        if state not in stats:
            continue
        s = stats[state]
        print(f"--- {state} (target: {entries[0][2] if state != 'EXHALE' else 5.0:.0f} cmH2O, "
              f"n={s['n']}) ---")
        print(f"  Pressure range:  {s['min_pressure']:.1f} – {s['max_pressure']:.1f} cmH2O")
        print(f"  Mean error:      {s['mean_error']:+.2f} cmH2O")
        print(f"  Mean |error|:    {s['mean_abs_error']:.2f} cmH2O")
        print(f"  Max  |error|:    {s['max_abs_error']:.2f} cmH2O")
        print(f"  RMS error:       {s['rms_error']:.2f} cmH2O")
        print(f"  Mean duty:       {s['mean_duty']:.0f}%")
        print()


if __name__ == "__main__":
    main()
