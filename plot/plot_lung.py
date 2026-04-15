#!/usr/bin/env python3
"""Plot lung alone vs lung + ventilator."""

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

    time, alone_p, alone_v, vent_p, vent_v, target = [], [], [], [], [], []
    with open(sys.argv[1]) as f:
        for row in csv.DictReader(f):
            time.append(float(row["time_s"]))
            alone_p.append(float(row["alone_pressure"]))
            alone_v.append(float(row["alone_volume"]))
            vent_p.append(float(row["vent_pressure"]))
            vent_v.append(float(row["vent_volume"]))
            target.append(float(row["target"]))

    fig, (ax1, ax2) = plt.subplots(2, 1, sharex=True, figsize=(10, 6))
    fig.suptitle("Lung Alone vs Lung + Ventilator (±5% noise)")

    # Pressure
    ax1.plot(time, alone_p, "r-", lw=1, alpha=0.7, label="Lung alone")
    ax1.plot(time, vent_p, "b-", lw=1, label="Lung + vent")
    ax1.plot(time, target, "k--", lw=0.8, alpha=0.5, label="Target")
    ax1.set_ylabel("Pressure (cmH₂O)")
    ax1.legend(loc="upper right", fontsize=8)
    ax1.grid(True, alpha=0.3)

    # Volume
    ax2.plot(time, alone_v, "r-", lw=1, alpha=0.7, label="Lung alone")
    ax2.plot(time, vent_v, "b-", lw=1, label="Lung + vent")
    ax2.set_ylabel("Volume (mL)")
    ax2.set_xlabel("Time (s)")
    ax2.legend(loc="upper right", fontsize=8)
    ax2.grid(True, alpha=0.3)

    plt.tight_layout()
    out_path = sys.argv[1].replace(".csv", ".png")
    plt.savefig(out_path, dpi=150)
    print(f"Saved {out_path}")
    plt.show()

if __name__ == "__main__":
    main()
