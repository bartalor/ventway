#!/usr/bin/env python3
"""Plot lung model CSV output — pressure and volume over time."""

import sys
import csv

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 plot_lung.py <csv_file>")
        sys.exit(1)

    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("Install matplotlib:  pip install matplotlib")
        sys.exit(1)

    time, pressure, volume, phase = [], [], [], []
    with open(sys.argv[1]) as f:
        for row in csv.DictReader(f):
            time.append(float(row["time_s"]))
            pressure.append(float(row["pressure_cmH2O"]))
            volume.append(float(row["volume_mL"]))
            phase.append(row["phase"])

    # Color background by phase
    phase_colors = {"inhale": "#d4edda", "hold": "#fff3cd", "exhale": "#f8d7da"}

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(10, 6))
    fig.suptitle("Lung Model — 3 Breath Cycles (±5% noise)")

    for ax in (ax1, ax2):
        i = 0
        while i < len(time):
            j = i
            while j < len(time) and phase[j] == phase[i]:
                j += 1
            ax.axvspan(time[i], time[min(j, len(time)-1)],
                       alpha=0.3, color=phase_colors.get(phase[i], "#eee"),
                       lw=0)
            i = j

    ax1.plot(time, pressure, "b-", lw=1)
    ax1.set_ylabel("Pressure (cmH₂O)")
    ax1.grid(True, alpha=0.3)

    ax2.plot(time, volume, "r-", lw=1)
    ax2.set_ylabel("Volume (mL)")
    ax2.set_xlabel("Time (s)")
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(sys.argv[1].replace(".csv", ".png"), dpi=150)
    print(f"Saved {sys.argv[1].replace('.csv', '.png')}")
    plt.show()

if __name__ == "__main__":
    main()
