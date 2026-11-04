#!/usr/bin/env python3
"""ab_time.py — paired wall-clock + Ir A/B for one binary on capture1.

Prints median wall seconds over --reps runs of the regression capture argv,
plus the decoded-packet digest (bit-exactness check between variants).

Use: python3 tools/ab_time.py bin/file_lora --reps 15
"""

import argparse
import hashlib
import statistics
import subprocess
import sys
import time

ARGV_TAIL = [
    "-s",
    "9",
    "-o",
    "8",
    "-a",
    "1",
    "--conj",
    "--center-freq",
    "869.525e6",
    "--bw",
    "250e3",
    "--input-rate",
    "1024000",
    "data/capture1/all_frames.cf32",
]


def run_once(binary):
    t0 = time.perf_counter()
    p = subprocess.run([binary] + ARGV_TAIL, capture_output=True, text=True)
    dt = time.perf_counter() - t0
    if p.returncode != 0:
        sys.exit(f"{binary} failed: {p.stderr[-500:]}")
    packets = [l for l in p.stdout.splitlines() if '"packet"' in l]
    dig = hashlib.sha256("\n".join(sorted(packets)).encode()).hexdigest()[:16]
    n = len(packets)
    return dt, dig, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--reps", type=int, default=15)
    a = ap.parse_args()

    d, dig, n = run_once(a.binary)  # warm-up, supplies the digest
    times = [run_once(a.binary)[0] for _ in range(a.reps)]
    med = statistics.median(times)
    print(f"{a.binary}: median {med:.3f} s  (min {min(times):.3f}, max {max(times):.3f})")
    print(f"packets={n} digest={dig}")


if __name__ == "__main__":
    main()
