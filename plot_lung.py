#!/usr/bin/env python3
"""
Plot lung model: open-loop vs closed-loop comparison.

Shows how connecting the lung to the Ventway PID controller
improves pressure tracking compared to a raw pressure square wave.
"""

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

    # Parse CSV into open-loop and closed-loop datasets
    data = {"open": {"time": [], "pressure": [], "volume": [], "target": [], "phase": []},
            "closed": {"time": [], "pressure": [], "volume": [], "target": [], "phase": []}}

    with open(sys.argv[1]) as f:
        for row in csv.DictReader(f):
            mode = row["mode"]
            data[mode]["time"].append(float(row["time_s"]))
            data[mode]["pressure"].append(float(row["pressure_cmH2O"]))
            data[mode]["volume"].append(float(row["volume_mL"]))
            data[mode]["target"].append(float(row["target_cmH2O"]))
            data[mode]["phase"].append(row["phase"])

    phase_colors = {"inhale": "#d4edda", "hold": "#fff3cd", "exhale": "#f8d7da"}

    fig, axes = plt.subplots(2, 2, sharex="col", figsize=(14, 7))
    fig.suptitle("Lung Model — Open-Loop vs Closed-Loop (±5% noise)", fontsize=13)

    titles = {"open": "Open-Loop (fixed pressure)", "closed": "Closed-Loop (Ventway PID)"}

    for col, mode in enumerate(["open", "closed"]):
        d = data[mode]
        ax_p = axes[0][col]
        ax_v = axes[1][col]

        # Phase background shading
        for ax in (ax_p, ax_v):
            i = 0
            while i < len(d["time"]):
                j = i
                while j < len(d["time"]) and d["phase"][j] == d["phase"][i]:
                    j += 1
                ax.axvspan(d["time"][i], d["time"][min(j, len(d["time"])-1)],
                           alpha=0.3, color=phase_colors.get(d["phase"][i], "#eee"), lw=0)
                i = j

        # Pressure: actual + target
        ax_p.plot(d["time"], d["pressure"], "b-", lw=1, label="Airway pressure")
        ax_p.plot(d["time"], d["target"], "k--", lw=0.8, alpha=0.6, label="Target")
        ax_p.set_ylabel("Pressure (cmH₂O)")
        ax_p.set_title(titles[mode])
        ax_p.legend(loc="upper right", fontsize=8)
        ax_p.grid(True, alpha=0.3)
        ax_p.set_ylim(-2, 35)

        # Volume
        ax_v.plot(d["time"], d["volume"], "r-", lw=1)
        ax_v.set_ylabel("Volume (mL)")
        ax_v.set_xlabel("Time (s)")
        ax_v.grid(True, alpha=0.3)

    plt.tight_layout()
    out_path = sys.argv[1].replace(".csv", ".png")
    plt.savefig(out_path, dpi=150)
    print(f"Saved {out_path}")
    plt.show()

if __name__ == "__main__":
    main()
