#!/usr/bin/env python3
"""c_fa.py — false-alarm bench: how often the detector locks onto pure noise.

References:
- src/detect.c "detect_mag_sum" — the sliding mean that makes consecutive
  decisions sticky, and so sets lambda
- arXiv:1905.11252 — the literature's framing of the same quantity
"""

import argparse
import concurrent.futures
import os
import shlex
import subprocess
import sys
import tempfile
import time

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
from common import add_awgn, max_jobs

BIN_ARGS = {
    # the DETECT-lock and sync lines are DEBUG, so the binary runs with -v;
    # "no frame detected" also lands on stderr, so anchor on the exact phrase
    "DETECT lock": "DEBUG: frame detected",
    "sync survive": "INFO: sync word ok",
    "header ok": "INFO: header: pay_len=",
    "CRC valid": "INFO: CRC valid!",
    "CRC absent": "INFO: CRC absent",
}


def noise_capture(path, n, sigma, seed, pad_only=False):
    """Write `n` samples of noise-only (or zero-only) IQ, as complex64.

    add_awgn scales by its input's mean power, so a unit-amplitude carrier
    carries the requested sigma in (-10*log10(2*sigma^2) SNR); the carrier is
    subtracted after — a leftover DC term is a tone the detector's argmax
    locks onto every window. The complex64 cast must be the last step before
    tofile: NEP-50 promotes complex64*float64 to complex128, and a widened
    file read back as complex64 hands the receiver double bit patterns.
    Asserted below."""
    if pad_only:
        y = np.zeros(n, dtype=np.complex64)
    else:
        carrier = np.ones(n, dtype=np.complex64)
        y = add_awgn(carrier, -10 * np.log10(2 * sigma**2), np.random.default_rng(seed))
        y = (y - carrier).astype(np.complex64)  # drop the carrier: DC is a tone
    y = y.astype(np.complex64)
    assert y.dtype == np.complex64 and y.nbytes == 8 * n, "capture must be 8 bytes/sample"
    p_meas = float(np.mean(np.abs(y) ** 2))
    assert pad_only or abs(p_meas - 2 * sigma**2) < 0.2 * sigma**2, (
        f"noise power {p_meas:.3f} is not the requested {2 * sigma**2:.3f}"
    )
    assert pad_only or abs(complex(y.mean())) < 0.05 * sigma, (
        f"capture mean {y.mean():.4f} is not zero — a DC term is a tone the detector locks onto"
    )
    y.tofile(path)
    return n


def run_once(bin_path, sf, avg, iq, extra=()):
    """One capture through the binary; which lock lines it printed."""
    cp = subprocess.run(
        [bin_path, "-v", "-s", str(sf), "-a", str(avg), *extra, iq],
        capture_output=True,
        text=True,
        errors="replace",
    )
    return {name: (needle in cp.stderr) for name, needle in BIN_ARGS.items()}


def run_noise_trial(job):
    """Pool worker: one noise draw through the binary."""
    bin_path, sf, avg, windows, seed, sigma, os_factor, pad_only, extra = job
    n = windows * (1 << sf) * os_factor
    with tempfile.TemporaryDirectory() as tmp:
        iq = os.path.join(tmp, "noise.cf32")
        noise_capture(iq, n, sigma, seed=seed, pad_only=pad_only)
        return run_once(bin_path, sf, avg, iq, extra)


def measure(bin_path, sf, avg, windows, trials, sigma, os_factor, pad_only, jobs, extra=()):
    """lambda for one (SF, -a, W) cell: locks per symbol window, with a bound."""
    counts = {name: 0 for name in BIN_ARGS}
    t0 = time.perf_counter()
    draws = [
        (bin_path, sf, avg, windows, t, sigma, os_factor, pad_only, tuple(extra))
        for t in range(trials)
    ]
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
        for hit in ex.map(run_noise_trial, draws, chunksize=1):
            for name, h in hit.items():
                counts[name] += h
    dt = time.perf_counter() - t0

    def rate(name):
        """-ln(1-k/n)/W, or the rule-of-three upper bound when nothing locked."""
        k = counts[name]
        if k == 0:
            return None, 3.0 / (trials * windows)
        p = k / trials
        p = min(p, 1 - 1e-12)
        return -np.log(1 - p) / windows, None

    return counts, rate, dt


def selftest(args):
    """Four invariants: requested noise power, zero mean, cf32 file width,
    and zeros never lock (the control arm)."""
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        n = 320 * (1 << args.sf[0]) * 8
        iq = os.path.join(tmp, "selftest.cf32")

        noise_capture(iq, n, args.sigma, seed=1)
        x = np.fromfile(iq, dtype=np.complex64)
        p_in, dc_in = float(np.mean(np.abs(x) ** 2)), abs(complex(x.mean()))
        want = 2 * args.sigma**2
        good = abs(p_in - want) < 0.2 * want
        ok &= good
        print(
            f"selftest noise power: {'PASS' if good else 'FAIL'}  "
            f"(measured {p_in:.2f}, want {want:.2f} = 2*sigma^2)"
        )

        quiet_carrier = dc_in < 0.05 * args.sigma
        ok &= quiet_carrier
        print(
            f"selftest zero mean:  {'PASS' if quiet_carrier else 'FAIL'}  "
            f"(|E[x]| = {dc_in:.4f}, must be << sigma = {args.sigma:.2f})"
        )

        width = os.path.getsize(iq) == 8 * n
        ok &= width
        print(
            f"selftest cf32 width: {'PASS' if width else 'FAIL'}  "
            f"({os.path.getsize(iq)} bytes for {n} samples, want {8 * n})"
        )

        noise_capture(iq, n, args.sigma, seed=1, pad_only=True)
        pad = run_once(args.bin, args.sf[0], args.a[0], iq, ())
        quiet = not any(pad.values())
        ok &= quiet
        print(f"selftest pad-only: {'PASS' if quiet else 'FAIL'}  (zeros must not lock: {pad})")
    return bool(ok)


def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "The detector's P_fa instrument, measured through the shipped "
            "binary so it prices the whole chain rather than a model of it:\n"
            "    lambda = spurious DETECT locks per symbol window of noise-only IQ\n"
            "TWO AXES, AND ONE THAT IS NOT AN AXIS AT ALL\n"
            "* Capture length in symbol windows (--windows) is the axis that "
            "matters: lambda is a rate, so P(lock within W windows) rises with "
            "W and short captures under-report it.\n"
            "* SF and -a (--sf, --a) set the rate itself. It rises steeply with "
            "averaging depth, which is the whole reason this bench exists.\n"
            "* sigma is NOT an axis. The decision is an argmax, hence "
            "scale-invariant: identical lock counts at every noise level. "
            "--sigma exists only so a capture can look realistic; sweeping it "
            "is a wasted run.\n"
            "A DETECT lock and a surviving SYNC (net-ID check passed) are "
            "counted separately because they cost different things; 'sync' is "
            "the number that reaches the message layer. --pad-only is the "
            "control arm: zeros never produce energy, so it must report zero "
            "locks — if it does not, the bench itself is broken."
        ),
    )
    p.add_argument("--sf", type=int, nargs="+", default=[7, 9, 12], help="spreading factors")
    p.add_argument("--a", type=int, nargs="+", default=[1, 4], help="receiver -a averaging depths")
    p.add_argument(
        "--windows", type=int, nargs="+", default=[40, 160], help="capture length in symbol windows"
    )
    p.add_argument("--trials", type=int, default=50, help="noise draws per cell")
    p.add_argument(
        "--jobs",
        type=int,
        default=max_jobs(),
        help="worker processes for the noise draws per cell (1 = serial)",
    )
    p.add_argument(
        "--sigma",
        type=float,
        default=3.1622776601683795,
        help="noise sigma per component (sqrt(10)); scale-invariant, not an axis",
    )
    p.add_argument(
        "--os", type=int, default=8, help="samples per chip (must match the binary's -o)"
    )
    p.add_argument("--pad-only", action="store_true", help="zeros instead of noise (control arm)")
    p.add_argument(
        "--bin", default=os.path.join(ROOT, "bin", "file_lora"), help="binary under test"
    )
    p.add_argument(
        "--bin-args",
        default="",
        metavar="ARGS",
        help="extra arguments for the binary, split like a shell command line. "
        "The noise draws are indexed by trial, not by flags, so any accept "
        "path a flag opens is priced against the exact same draws",
    )
    p.add_argument(
        "--selftest", action="store_true", help="check the control arm, exit 1 on failure"
    )
    args = p.parse_args()
    args.bin_argv = shlex.split(args.bin_args)
    if not os.access(args.bin, os.X_OK):
        p.error(f"{args.bin} missing or not executable (run make)")

    if args.selftest:
        sys.exit(0 if selftest(args) else 1)

    print(
        f"false-alarm bench  bin={args.bin}  trials/cell={args.trials}  "
        f"os={args.os}  sigma={args.sigma:g} (scale-invariant: not an axis)\n"
        f"lambda = locks per symbol window; '<' is a 95% upper bound (rule of three)"
    )
    short = {
        "DETECT lock": "detect",
        "sync survive": "sync",
        "header ok": "header",
        "CRC valid": "crc_ok",
        "CRC absent": "crc_abs",
    }
    header = (
        f"{'SF':>3} {'-a':>3} {'W':>5} "
        + " ".join(f"{short[n]:>7}" for n in BIN_ARGS)
        + "  "
        + " ".join(f"{'lam_' + short[n]:>11}" for n in BIN_ARGS)
    )
    print(header)
    print("-" * len(header))

    for sf in args.sf:
        for avg in args.a:
            for w in args.windows:
                counts, rate, dt = measure(
                    args.bin,
                    sf,
                    avg,
                    w,
                    args.trials,
                    args.sigma,
                    args.os,
                    args.pad_only,
                    args.jobs,
                    args.bin_argv,
                )
                cells = []
                for name in BIN_ARGS:
                    val, bound = rate(name)
                    cells.append(f"<{bound:.1e}" if val is None else f"{val:.2e}")
                print(
                    f"{sf:>3} {avg:>3} {w:>5} "
                    + " ".join(f"{counts[n]:>7}" for n in BIN_ARGS)
                    + "  "
                    + " ".join(f"{c:>11}" for c in cells)
                    + f"   ({dt:.1f}s)"
                )
    print(
        "\nA rising lambda with W means the cell is still short: rate needs\n"
        "capture length to converge. Compare cells at equal W only."
    )


if __name__ == "__main__":
    main()
