#!/usr/bin/env python3
"""regression_perf.py — the performance twin of regression.py: same verdicts,
but the number beside each is speed (RTx = iq samples of IQ per wall second)."""

import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import frame as frame_mod
from common import add_awgn

SYNC = [8, 16]  # the 0x12 private sync word, as hardcoded in main.c


@dataclass
class Case:
    """One matrix row: the capture to build, the argv to run, the verdict."""

    label: str
    sf: int = 7
    os: int = 8
    bw: float = 125e3  # time scaling of the capture (fs = bw*os), not the DSP
    cr: int = 1  # coding rate 4/(4+cr)
    pl: int = 64  # payload bytes
    snr: float | None = None  # AWGN bed level [dB]; None = clean capture
    pre_s: float = 0.0  # seconds of noise before the frame (guard-time walk)
    post_s: float = 0.0  # seconds of noise after it (detect stops at the frame)
    avg: int = 4  # receiver -a (DETECT averaging depth)
    pre: int = 8  # receiver -p: preamble upchirps required (n_up_req = pre-3)
    pre_gen: int = 0  # preamble upchirps in the capture; 0 = same as `pre`
    sto: int = 300  # injected STO, samples
    cfo: float = -4.7  # injected CFO, FFT bins

    @property
    def fs(self):
        """Capture sample rate [Hz] — the file's samples per airtime second."""
        return self.bw * self.os

    def cap_key(self):
        """Identity of the cf32 file this case needs (bw and avg are argv-only,
        so those rows share a file)."""
        return (
            self.sf,
            self.os,
            self.cr,
            self.pl,
            self.snr,
            self.pre_s,
            self.post_s,
            self.pre_gen or self.pre,
            self.sto,
            self.cfo,
        )


class CaseFail(Exception):
    """A verdict failure, carrying the reason and the captured output tail."""

    def __init__(self, why, out):
        super().__init__(why)
        self.why = why
        self.out = out


def noise_bed(n, p_sig, snr_db, rng):
    """`n` noise-only samples at `snr_db` relative to power `p_sig` (a constant
    dummy carries the power, so add_awgn's average estimate is exact)."""
    if n <= 0:
        return np.zeros(0, dtype=np.complex64)
    dummy = np.full(n, np.sqrt(p_sig), dtype=np.complex64)
    return add_awgn(dummy, snr_db, rng)


def build_capture(case: Case, path: str, seed: int = 1):
    """Write the case's cf32 capture; return (n_samples, truth payload bytes).
    Noise is concatenated around the frame, never summed over it, so the
    verdict stays deterministic."""
    rng = np.random.default_rng(seed)
    sps = (1 << case.sf) * case.os
    truth = rng.integers(0, 256, case.pl, dtype=np.uint8).tobytes()
    frame, _ = frame_mod.generate(
        sf=case.sf,
        os_=case.os,
        pre=case.pre_gen or case.pre,
        sync=SYNC,
        msg=truth,
        cr=case.cr,
        has_crc=True,
        sto=case.sto,
        cfo=case.cfo,
    )
    # file plumbing, not DSP: trailing zeros keep reads in bounds at the frame end
    parts = [frame, np.zeros(sps // 2, dtype=frame.dtype)]
    if case.snr is not None:
        p_sig = float(np.mean(np.abs(frame) ** 2))
        parts.insert(0, noise_bed(round(case.pre_s * case.fs), p_sig, case.snr, rng))
        parts.append(noise_bed(round(case.post_s * case.fs), p_sig, case.snr, rng))
    x = np.concatenate(parts)
    x.astype(np.complex64).tofile(path)
    return len(x), truth


def case_cmd(case: Case, bin_path: str, iq: str):
    """The measured argv: the case's receiver options, then the capture."""
    return [
        bin_path,
        "-s",
        str(case.sf),
        "-o",
        str(case.os),
        "-p",
        str(case.pre),
        "-a",
        str(case.avg),
        iq,
    ]


def score(case: Case, out: str, truth):
    """Score the receiver's JSONL frame records exactly as regression.py does."""
    recs = [json.loads(line) for line in out.splitlines() if line.strip()]
    if not recs:
        raise CaseFail("no frame decoded", out)
    rec = recs[0]
    got = base64.b64decode(rec["packet"])
    ok = rec["crc_valid"] is True
    if got != truth or not ok:
        crc = "valid" if ok else ("absent" if not rec["crc_present"] else "INVALID")
        raise CaseFail(f"expected {len(truth)} B + CRC valid; got {len(got)} B, CRC {crc}", out)
    return f"{len(truth)} B exact, CRC valid"


def timed_run(cmd, reps):
    """Best and median wall time over `reps` runs; the first run is the warm-up."""
    subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    ts, out = [], ""
    for _ in range(reps):
        t0 = time.perf_counter()
        cp = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        ts.append(time.perf_counter() - t0)
        out = cp.stdout
    ts.sort()
    return ts[0], ts[len(ts) // 2], out


def measure_floor(bin_path, tmp, reps):
    """Process start-up cost (exec, dynamic linking, stdio): the part of
    every run that is not processing."""
    iq = os.path.join(tmp, "floor.cf32")
    np.zeros(4, dtype=np.complex64).tofile(iq)
    best, _, _ = timed_run([bin_path, "-s", "7", iq], reps)
    return best


def build_cases(seconds: float, snr: float):
    """The matrix: (section, Case) rows over both bottlenecks."""
    rows = []

    # 1. acquisition: a --seconds noise bed precedes the frame, so DETECT
    # walks all its windows first. SF12 only: shorter symbols lock on the
    # bed noise too fast to measure a clean walk
    for mult in (1, 2):
        rows.append(
            (
                f"acquisition (SF12, {mult * seconds:g}s noise bed @ {snr:g} dB, then frame)",
                Case(
                    f"sf12 64 B after {mult * seconds:g}s bed", sf=12, snr=snr, pre_s=mult * seconds
                ),
            )
        )

    # 2. decode: clean frames, DEMOD/FFT-bound. 255 B is the protocol maximum
    # and the only clean-frame size whose airtime clears the process floor
    for sf in range(7, 13):
        rows.append(
            ("decode (clean frame, FFT/DEMOD-bound)", Case(f"sf{sf} 255 B cr4/5", sf=sf, pl=255))
        )
    # payload-size ladder at SF9: the same frame, more or less of it
    for pl in (12, 64, 255):
        rows.append(
            ("decode (clean frame, FFT/DEMOD-bound)", Case(f"sf9 pl {pl:3d} B cr4/5", sf=9, pl=pl))
        )
    # coding-rate ladder at SF9: same payload, more symbols per byte
    for cr in (1, 2, 3, 4):
        rows.append(
            ("decode (clean frame, FFT/DEMOD-bound)", Case(f"sf9 64 B  cr4/{cr + 4}", sf=9, cr=cr))
        )

    # 3. receiver options at a fixed SF and payload; the capture carries 24
    # preamble upchirps so even -p 16 has room for its K <= pre - n_up_req + 1
    # averaging budget (main.c)
    for avg in (1, 2, 4, 8):
        rows.append(("receiver options", Case(f"sf9 64 B  -a {avg}", sf=9, avg=avg)))
    for os_ in (4, 8, 16):
        rows.append(("receiver options", Case(f"sf9 64 B  -o {os_}", sf=9, os=os_)))
    for pre in (8, 16):
        rows.append(("receiver options", Case(f"sf9 64 B  -p {pre}", sf=9, pre=pre, pre_gen=24)))

    # 4. bandwidth: one capture, fs = bw*os, so only the RTx denominator moves
    for bw in (125e3, 250e3, 500e3):
        rows.append(
            (
                "bandwidth (time scaling of one capture)",
                Case(f"sf9 64 B  bw {bw / 1e3:g} kHz", sf=9, bw=bw),
            )
        )
    return rows


def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "RTx = (iq samples / fs) / wall seconds, fs = bw * os. bw is "
            "file-mode time scaling only (ber.py's convention), which is "
            "why the bandwidth rows reuse one capture and only rescale RTx. "
            "Wall time is the whole subprocess — process start, capture "
            "read, detect, sync, demod, decode. Each case runs one warm-up "
            "(which supplies the verdict) plus --reps timed runs; best is "
            "the headline, median sits beside it. Process start-up is "
            "measured once on a 4-sample file ('floor'); the 'start' column "
            "is its share of each run, so a short frame's RTx is visibly a "
            "floor, not a throughput.\n"
            "The matrix spans the two bottlenecks a live receiver has: "
            "acquisition (noise bed of --seconds before the frame, DETECT "
            "walking all its windows; SF12 only, shorter symbols false-lock "
            "on the bed) and decode (clean frames, over SF, payload size, "
            "coding rate and -a/-o/-p).\n"
            "The exit code is the verdict, never the speed: a fast case "
            "that stops decoding is a FAIL."
        ),
    )
    p.add_argument("--reps", type=int, default=3, help="timed runs per case (default 3)")
    p.add_argument(
        "--seconds",
        type=float,
        default=2.0,
        help="seconds of noise in the acquisition bed (default 2)",
    )
    p.add_argument("--snr", type=float, default=-10.0, help="noise-bed SNR in dB (default -10)")
    p.add_argument("--only", default=None, help="run only labels containing this substring")
    p.add_argument("--list", action="store_true", help="print the matrix and exit")
    p.add_argument(
        "--bin",
        default=os.path.join(ROOT, "bin", "file_lora"),
        help="path to the built C binary under test (not built here)",
    )
    p.add_argument(
        "--selftest",
        action="store_true",
        help="a short 3-case run on small captures, exit 1 on any failure",
    )
    args = p.parse_args()
    if not os.access(args.bin, os.X_OK):
        p.error(f"{args.bin} missing or not executable (build it first)")
    bin_path = os.path.abspath(args.bin)

    seconds = 0.5 if args.selftest else args.seconds
    reps = 1 if args.selftest else max(1, args.reps)
    rows = build_cases(seconds, args.snr)
    if args.selftest:
        want = ("sf12 64 B after 0.5s bed", "sf7 255 B cr4/5")
        rows = [r for r in rows if r[1].label in want]
    if args.only:
        rows = [r for r in rows if args.only in r[1].label]
    if not rows:
        p.error("no cases match --only")

    if args.list:
        for sec, c in rows:
            print(f"{sec:<46} {c.label}")
        return

    print(
        f"file_lora perf  seed=1  {len(rows)} cases  reps={reps}  bin={args.bin}\n"
        f"RTx = (iq samples / (bw*os)) / wall seconds; wall = the whole "
        f"subprocess (start + read + detect + demod + decode)"
    )

    n_fail = warned = 0
    rts = []
    with tempfile.TemporaryDirectory() as tmp:
        floor = measure_floor(bin_path, tmp, reps)
        print(f"process floor: {floor * 1e3:.2f} ms (4-sample file, same argv shape)")
        captures = {}  # cap_key -> (path, n_samples, truth)
        section = None
        for i, (sec, case) in enumerate(rows):
            if sec != section:
                section = sec
                print(f"\n== {sec} ==")
            key = case.cap_key()
            if key not in captures:
                iq = os.path.join(tmp, f"cap{i:02d}.cf32")
                n, truth = build_capture(case, iq)
                captures[key] = (iq, n, truth)
            iq, n, truth = captures[key]
            cmd = case_cmd(case, bin_path, iq)
            try:
                best, med, out = timed_run(cmd, reps)
                note = score(case, out, truth)
            except CaseFail as e:
                n_fail += 1
                print(f"FAIL  {case.label:<26} {'':>30}  {e.why}")
                for line in e.out.strip().splitlines()[-8:]:
                    print(f"      | {line}")
                continue
            rt = (n / case.fs) / best
            rts.append((rt, case.label))
            start = floor / best  # how much of the run was process start-up
            if start > 0.25:
                warned += 1
            print(
                f"PASS  {case.label:<26} {n / case.fs:6.2f}s IQ {best * 1e3:7.1f}/"
                f"{med * 1e3:6.1f}ms  {rt:8.0f}x RT  {note}  (start {start * 100:3.0f}%)"
            )

    print("\n----")
    if rts:
        worst, best = min(rts), max(rts)
        print(
            f"{len(rows) - n_fail}/{len(rows)} passed;  RTx across the matrix: "
            f"worst {worst[0]:.0f}x ({worst[1]}), best {best[0]:.0f}x ({best[1]})"
        )
    if warned:
        print(
            f"{warned} case(s) spend >25% of the run in process start-up "
            f"(the `start` column) — their RTx is a floor, not a throughput"
        )
    sys.exit(0 if n_fail == 0 else 1)


if __name__ == "__main__":
    main()
