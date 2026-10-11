#!/usr/bin/env python3
"""
serial_logger.py -- capture the sink's "#CSV," stream into results/.

Every node emits two interleaved streams on one UART: a human-readable log for
the live demonstration, and machine-readable rows prefixed "#CSV,". This script
picks out the second, splits it by record type, and writes one CSV per type so
plot_metrics.py can graph them.

Run it against the SINK (N4) for the delivery metrics, or against any node to
capture that node's MAC and BLE records.

Usage:
    python tools/serial_logger.py --port COM7
    python tools/serial_logger.py --port COM7 --seconds 300
    python tools/serial_logger.py --list
"""

import argparse
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"

# record type -> column names (the prefix "node,uptime_ms" is added to all)
SCHEMA = {
    "PERF":       ["pdr", "rx", "lost", "delay_ms", "throughput_bps",
                   "retx", "reduction_pct"],
    "RX":         ["src", "seq", "hops", "frame_bytes", "delay_ms", "path"],
    "TX":         ["type", "seq", "src", "dst", "final_dst", "bytes", "retry"],
    "ACK":        ["from_node", "seq", "rtt_ms", "retries"],
    "RETX":       ["dst", "seq", "retry", "cw"],
    "DROP":       ["dst", "seq", "retries"],
    "BACKOFF":    ["slots", "cw"],
    "BLE":        ["node", "rssi_dbm", "samples", "dist_est_m", "dist_err_m"],
    "LOC":        ["est_x", "est_y", "true_x", "true_y", "error_m", "rmse_m",
                   "anchors"],
    "AGG":        ["raw_packets", "raw_bytes", "agg_bytes",
                   "byte_reduction_pct", "packet_reduction_pct"],
    "SCHED":      ["version", "reason_code", "reason_node"],
    "ROUTE":      ["old_next", "new_next", "hops", "cost"],
    "LINKDOWN":   ["peer", "consec_fail"],
    "SLOTUNUSED": ["slot"],
    "CALIB":      ["sensor", "rs_clean_kohm", "r0_kohm"],
}


def list_serial_ports():
    ports = list(list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return
    print("Available serial ports:")
    for p in ports:
        print(f"  {p.device:10s}  {p.description}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="e.g. COM7 or /dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=int, default=0,
                    help="stop after N seconds (0 = until Ctrl-C)")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--quiet", action="store_true",
                    help="do not echo the human-readable log")
    args = ap.parse_args()

    if args.list:
        list_serial_ports()
        return 0
    if not args.port:
        list_serial_ports()
        sys.exit("\nerror: --port is required")

    RESULTS.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = RESULTS / f"run_{stamp}"
    run_dir.mkdir()

    writers, handles = {}, {}
    raw = open(run_dir / "serial_raw.log", "w", encoding="utf-8", errors="replace")

    def writer_for(kind, ncols):
        if kind not in writers:
            f = open(run_dir / f"{kind.lower()}.csv", "w", newline="", encoding="utf-8")
            cols = SCHEMA.get(kind)
            if cols is None:
                # Unknown record type: keep it rather than discard it, with
                # generic column names, so a firmware change does not silently
                # lose data mid-experiment.
                cols = [f"f{i}" for i in range(ncols)]
            w = csv.writer(f)
            w.writerow(["node", "uptime_ms"] + cols)
            writers[kind], handles[kind] = w, f
        return writers[kind]

    print(f"Logging {args.port} @ {args.baud} -> {run_dir}")
    print("Press Ctrl-C to stop.\n")

    counts = {}
    t0 = time.time()
    try:
        with serial.Serial(args.port, args.baud, timeout=1) as ser:
            while True:
                if args.seconds and (time.time() - t0) >= args.seconds:
                    break
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                raw.write(line + "\n")

                if not line.startswith("#CSV,"):
                    if not args.quiet:
                        print(line)
                    continue

                parts = line[5:].split(",")
                if len(parts) < 3:
                    continue
                node, uptime, kind = parts[0], parts[1], parts[2]
                fields = parts[3:]
                writer_for(kind, len(fields)).writerow([node, uptime] + fields)
                counts[kind] = counts.get(kind, 0) + 1

    except KeyboardInterrupt:
        print("\nStopped.")
    except serial.SerialException as e:
        sys.exit(f"\nserial error: {e}")
    finally:
        raw.close()
        for f in handles.values():
            f.close()

    print(f"\nCaptured {sum(counts.values())} records in {time.time()-t0:.0f} s:")
    for k in sorted(counts):
        print(f"   {k:<12} {counts[k]}")
    print(f"\nWritten to {run_dir}")
    print(f"Next:  python tools/plot_metrics.py --run {run_dir.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
