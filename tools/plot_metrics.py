#!/usr/bin/env python3
"""
plot_metrics.py -- turn a captured run into the graphs and tables the report
needs.

Reads the CSVs written by serial_logger.py and produces, in the same run
folder:

    pdr_over_time.png        delivery ratio and cumulative loss
    delay_distribution.png   end-to-end delay histogram + time series
    throughput.png           application throughput over the run
    mac_activity.png         backoff distribution and retransmissions
    localization.png         error per round, with mean and RMSE
    aggregation.png          raw vs aggregated bytes, and reduction %
    summary.md               a results table ready to paste into the report

Only the plots whose source data exists are produced, so a partial capture
still yields something useful.

Usage:
    python tools/plot_metrics.py                 # newest run
    python tools/plot_metrics.py --run run_20261011_143000
    python tools/plot_metrics.py --list
"""

import argparse
import sys
from pathlib import Path

try:
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("pandas and matplotlib are required:\n"
             "    pip install pandas matplotlib")

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

plt.rcParams.update({
    "figure.dpi": 130,
    "savefig.dpi": 130,
    "font.size": 9,
    "axes.grid": True,
    "grid.alpha": 0.3,
    "axes.spines.top": False,
    "axes.spines.right": False,
})

ACCENT, OK, WARN, BAD = "#2f6fb0", "#3a9a52", "#c99320", "#c0392b"


def load(run: Path, name: str):
    p = run / f"{name}.csv"
    if not p.exists():
        return None
    try:
        df = pd.read_csv(p)
    except Exception as e:
        print(f"   ! {name}.csv unreadable ({e})")
        return None
    if df.empty:
        return None
    if "uptime_ms" in df:
        df["t"] = (df["uptime_ms"] - df["uptime_ms"].iloc[0]) / 1000.0
    return df


def save(fig, run: Path, name: str):
    out = run / name
    fig.tight_layout()
    fig.savefig(out)
    plt.close(fig)
    print(f"   + {name}")


# --------------------------------------------------------------------------- #

def plot_pdr(run, perf):
    fig, (a, b) = plt.subplots(2, 1, figsize=(7, 5), sharex=True)
    a.plot(perf["t"], perf["pdr"] * 100, color=ACCENT, lw=1.6)
    a.set_ylabel("PDR (%)")
    a.set_ylim(0, 105)
    a.set_title("Packet delivery ratio and cumulative loss")
    a.axhline(100, color=OK, ls=":", lw=1)

    b.plot(perf["t"], perf["lost"], color=BAD, lw=1.6)
    b.fill_between(perf["t"], perf["lost"], color=BAD, alpha=0.15)
    b.set_ylabel("packets lost")
    b.set_xlabel("time (s)")
    save(fig, run, "pdr_over_time.png")


def plot_delay(run, rx, perf):
    fig, (a, b) = plt.subplots(1, 2, figsize=(8.5, 3.4))
    if rx is not None and "delay_ms" in rx:
        d = rx["delay_ms"][rx["delay_ms"] > 0]
        if len(d):
            a.hist(d, bins=24, color=ACCENT, edgecolor="white")
            a.axvline(d.mean(), color=BAD, ls="--", lw=1.4,
                      label=f"mean {d.mean():.0f} ms")
            a.legend(frameon=False)
    a.set_xlabel("end-to-end delay (ms)")
    a.set_ylabel("packets")
    a.set_title("Delay distribution")

    if perf is not None:
        b.plot(perf["t"], perf["delay_ms"], color=ACCENT, lw=1.5)
    b.set_xlabel("time (s)")
    b.set_ylabel("mean delay (ms)")
    b.set_title("Delay over the run")
    save(fig, run, "delay_distribution.png")


def plot_throughput(run, perf):
    fig, ax = plt.subplots(figsize=(7, 3.2))
    ax.plot(perf["t"], perf["throughput_bps"], color=OK, lw=1.6)
    ax.fill_between(perf["t"], perf["throughput_bps"], color=OK, alpha=0.15)
    ax.set_xlabel("time (s)")
    ax.set_ylabel("bit/s (application payload)")
    ax.set_title("Throughput at the sink")
    save(fig, run, "throughput.png")


def plot_mac(run, backoff, retx, perf):
    fig, (a, b) = plt.subplots(1, 2, figsize=(8.5, 3.4))
    if backoff is not None:
        a.hist(backoff["slots"], bins=range(0, int(backoff["slots"].max()) + 2),
               color=ACCENT, edgecolor="white", align="left")
        a.set_title(f"Random backoff (mean {backoff['slots'].mean():.2f} slots)")
    else:
        a.set_title("Random backoff (no data)")
    a.set_xlabel("backoff (slots)")
    a.set_ylabel("transmissions")

    if retx is not None:
        b.plot(retx["t"], range(1, len(retx) + 1), color=WARN, lw=1.6,
               label="retransmissions")
        if "cw" in retx:
            b2 = b.twinx()
            b2.step(retx["t"], retx["cw"], color=BAD, lw=1.2, where="post",
                    alpha=0.7)
            b2.set_ylabel("contention window", color=BAD)
            b2.grid(False)
        b.legend(frameon=False, loc="upper left")
    elif perf is not None:
        b.plot(perf["t"], perf["retx"], color=WARN, lw=1.6)
    b.set_xlabel("time (s)")
    b.set_ylabel("cumulative retransmissions")
    b.set_title("Retransmissions and backoff growth")
    save(fig, run, "mac_activity.png")


def plot_localization(run, loc):
    fig, (a, b) = plt.subplots(1, 2, figsize=(8.5, 3.6))
    rounds = range(1, len(loc) + 1)
    a.plot(rounds, loc["error_m"], "o-", color=ACCENT, lw=1.4, ms=4,
           label="per round")
    a.axhline(loc["error_m"].mean(), color=OK, ls="--", lw=1.3,
              label=f"mean {loc['error_m'].mean():.2f} m")
    if "rmse_m" in loc:
        a.plot(rounds, loc["rmse_m"], color=WARN, lw=1.3, label="RMSE")
    a.set_xlabel("localization round")
    a.set_ylabel("error (m)")
    a.set_title("Localization error")
    a.legend(frameon=False)

    b.scatter(loc["true_x"], loc["true_y"], s=70, facecolors="none",
              edgecolors=OK, lw=1.8, label="ground truth")
    b.scatter(loc["est_x"], loc["est_y"], s=22, color=WARN, alpha=0.75,
              label="estimated")
    for _, r in loc.iterrows():
        b.plot([r["true_x"], r["est_x"]], [r["true_y"], r["est_y"]],
               color="#999", lw=0.6, zorder=0)
    b.set_xlabel("x (m)")
    b.set_ylabel("y (m)")
    b.set_title("Estimated vs true position")
    b.set_aspect("equal", adjustable="datalim")
    b.legend(frameon=False)
    save(fig, run, "localization.png")


def plot_aggregation(run, agg):
    fig, (a, b) = plt.subplots(1, 2, figsize=(8.5, 3.4))
    a.plot(agg["t"], agg["raw_bytes"].cumsum(), color=BAD, lw=1.6,
           label="without aggregation")
    a.plot(agg["t"], agg["agg_bytes"].cumsum(), color=OK, lw=1.6,
           label="with aggregation")
    a.fill_between(agg["t"], agg["agg_bytes"].cumsum(),
                   agg["raw_bytes"].cumsum(), color=OK, alpha=0.12)
    a.set_xlabel("time (s)")
    a.set_ylabel("cumulative bytes on air")
    a.set_title("Traffic saved by aggregation")
    a.legend(frameon=False)

    b.plot(agg["t"], agg["byte_reduction_pct"], color=ACCENT, lw=1.6,
           label="bytes")
    if "packet_reduction_pct" in agg:
        b.plot(agg["t"], agg["packet_reduction_pct"], color=WARN, lw=1.4,
               label="packets")
    b.set_xlabel("time (s)")
    b.set_ylabel("reduction (%)")
    b.set_ylim(0, 100)
    b.set_title("Data reduction")
    b.legend(frameon=False)
    save(fig, run, "aggregation.png")


# --------------------------------------------------------------------------- #

def write_summary(run, data):
    perf, rx, loc, agg, backoff, retx, drop = (
        data.get(k) for k in ("perf", "rx", "loc", "agg", "backoff", "retx", "drop"))

    rows = []
    if perf is not None:
        last = perf.iloc[-1]
        rows += [
            ("1. Packet Delivery Ratio", f"{last['pdr']*100:.2f} %",
             f"{int(last['rx'])} delivered / {int(last['rx'] + last['lost'])} expected"),
            ("2. Packet loss", f"{int(last['lost'])} packets",
             f"{(1-last['pdr'])*100:.2f} % of expected"),
            ("4. Throughput", f"{last['throughput_bps']:.1f} bit/s",
             "application payload at the sink"),
            ("5. Retransmissions", f"{int(last['retx'])}",
             "reported by the sources"),
            ("7. Data reduction", f"{last['reduction_pct']:.2f} %",
             "bytes saved by aggregation"),
        ]
    if rx is not None and "delay_ms" in rx:
        d = rx["delay_ms"][rx["delay_ms"] > 0]
        if len(d):
            rows.insert(2, ("3. End-to-end delay",
                            f"{d.mean():.0f} ms mean",
                            f"min {d.min():.0f} / max {d.max():.0f} / "
                            f"median {d.median():.0f} ms, n={len(d)}"))
    if loc is not None:
        rows.append(("6. Localization error",
                     f"{loc['error_m'].mean():.2f} m mean",
                     f"RMSE {loc['rmse_m'].iloc[-1]:.2f} m over "
                     f"{len(loc)} rounds, min {loc['error_m'].min():.2f} / "
                     f"max {loc['error_m'].max():.2f} m"))

    lines = ["# Measured results", "",
             f"Run: `{run.name}`", "",
             "| Metric | Value | Detail |", "|---|---|---|"]
    lines += [f"| {m} | **{v}** | {d} |" for m, v, d in rows]

    if backoff is not None:
        lines += ["", "## MAC behaviour", "",
                  "| Quantity | Value |", "|---|---|",
                  f"| Transmissions with a backoff draw | {len(backoff)} |",
                  f"| Mean backoff | {backoff['slots'].mean():.2f} slots |",
                  f"| Max backoff | {int(backoff['slots'].max())} slots |",
                  f"| Max contention window reached | {int(backoff['cw'].max())} |"]
        if retx is not None:
            lines.append(f"| Retransmission events | {len(retx)} |")
        if drop is not None:
            lines.append(f"| Frames dropped at the retry limit | {len(drop)} |")

    if agg is not None:
        lines += ["", "## Aggregation", "",
                  "| Quantity | Value |", "|---|---|",
                  f"| Aggregation events | {len(agg)} |",
                  f"| Raw packets folded in | {int(agg['raw_packets'].sum())} |",
                  f"| Bytes without aggregation | {int(agg['raw_bytes'].sum())} |",
                  f"| Bytes with aggregation | {int(agg['agg_bytes'].sum())} |",
                  f"| Byte reduction | {agg['byte_reduction_pct'].iloc[-1]:.2f} % |",
                  f"| Packet reduction | {agg['packet_reduction_pct'].iloc[-1]:.2f} % |"]

    lines += ["", "## Figures", ""]
    for png in sorted(run.glob("*.png")):
        lines.append(f"![{png.stem}]({png.name})")
        lines.append("")

    (run / "summary.md").write_text("\n".join(lines), encoding="utf-8")
    print("   + summary.md")


# --------------------------------------------------------------------------- #

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", help="run folder name under results/")
    ap.add_argument("--list", action="store_true", help="list captured runs")
    args = ap.parse_args()

    runs = sorted(p for p in RESULTS.glob("run_*") if p.is_dir())
    if args.list:
        if not runs:
            print("No runs captured yet. Use tools/serial_logger.py first.")
        for r in runs:
            print(" ", r.name)
        return 0

    if args.run:
        run = RESULTS / args.run
        if not run.is_dir():
            sys.exit(f"error: no such run: {run}")
    elif runs:
        run = runs[-1]
    else:
        sys.exit("error: no runs found. Capture one with tools/serial_logger.py")

    print(f"Processing {run.name}")
    data = {k: load(run, k) for k in
            ("perf", "rx", "loc", "agg", "backoff", "retx", "drop")}

    if all(v is None for v in data.values()):
        sys.exit("error: no usable CSV data in this run.")

    if data["perf"] is not None:
        plot_pdr(run, data["perf"])
        plot_throughput(run, data["perf"])
    if data["rx"] is not None or data["perf"] is not None:
        plot_delay(run, data["rx"], data["perf"])
    if data["backoff"] is not None or data["retx"] is not None:
        plot_mac(run, data["backoff"], data["retx"], data["perf"])
    if data["loc"] is not None:
        plot_localization(run, data["loc"])
    if data["agg"] is not None:
        plot_aggregation(run, data["agg"])

    write_summary(run, data)
    print(f"\nDone. Figures and summary.md are in {run}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
