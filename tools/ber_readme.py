#!/usr/bin/env python3
"""The README BER plot: measured + theory curves for every SF on one axes.

Wraps ber.py: runs one sweep per SF (SNR windows pre-centered per SF, so
no time is wasted on floor/ceiling points), parses the logs with
ber_combine.parse and draws them the README way — one color per SF,
measured solid with round markers, uncoded theory dashed, coded theory
dotted, no titles or subtitles. Axes fit the measured points.

example:
  python3 tools/ber_readme.py --trials 20 --snr-step 2 --out ber-readme.png
  # per-SF trials: default 250, but 500 for SF9, 1000 for SF12
  python3 tools/ber_readme.py --trials '250, 9:500, 12:1000' --out ber-readme.png
  # reuse saved sweep logs instead of running:
  python3 tools/ber_readme.py --out ber-readme.png s7.log s9.log s12.log
"""

import argparse
import os
import subprocess
import sys

import matplotlib

matplotlib.use("Agg")
import re

import matplotlib.pyplot as plt
import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import ber_combine
from ber import theory_ber, theory_ber_coded

CR = re.compile(r"coded 4/(\d)")

# per-SF default SNR windows [dB]: each curve's waterfall, roughly
# center -12 dB (SF7) sliding 2.5-3 dB lower per SF step
RANGE = {7: (-15, -6), 8: (-18, -9), 9: (-20, -11), 10: (-23, -15), 11: (-25, -18), 12: (-28, -21)}
COLORS = ["tab:blue", "tab:orange", "tab:green", "tab:red", "tab:purple", "tab:brown"]


def parse_trials(spec: str, sfs: list[int]) -> dict[int, int]:
    """'250' or '250, 9:500, 12:1000' -> {sf: trials}.

    A bare number is the default for every SF; 'SF:N' overrides one SF."""
    trials = {}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if ":" in item:
            sf, n = item.split(":")
            trials[int(sf)] = int(n)
        else:
            trials.update({sf: int(item) for sf in sfs})
    return trials


def parse_sfs(spec: str) -> list[int]:
    """'7-12' or '7,9,12' -> sorted unique SF list."""
    if "-" in spec:
        lo, hi = spec.split("-")
        return list(range(int(lo), int(hi) + 1))
    return sorted({int(x) for x in spec.split(",")})


def run_sweeps(args) -> list[dict]:
    """One ber.py subprocess per SF; return its parsed sweep dicts."""
    import shlex

    logs = []
    trials = parse_trials(args.trials, parse_sfs(args.sfs))
    for sf in parse_sfs(args.sfs):
        lo, hi = RANGE[sf]
        if args.snr_min is not None:
            lo = args.snr_min
        if args.snr_max is not None:
            hi = args.snr_max
        cmd = [
            sys.executable,
            os.path.join(TOOLS, "ber.py"),
            "--sf",
            str(sf),
            "--os",
            str(args.os),
            "--snr-min",
            str(lo),
            "--snr-max",
            str(hi),
            "--snr-step",
            str(args.snr_step),
            "--trials",
            str(trials[sf]),
            "--seed",
            str(args.seed),
            "--cr",
            str(args.cr),
            "--pl",
            str(args.pl),
            "--jobs",
            str(args.jobs),
        ]
        cmd += shlex.split(args.bin_args)
        print(f"=== SF{sf}: {' '.join(cmd[2:])}", file=sys.stderr, flush=True)
        cp = subprocess.run(cmd, capture_output=True, text=True)
        sys.stderr.write(cp.stderr)
        if cp.returncode != 0:
            raise SystemExit(f"SF{sf} sweep failed ({cp.returncode})")
        log = os.path.join(args.tmpdir, f"sf{sf}.log")
        with open(log, "w") as fh:
            fh.write(cp.stdout)
        logs.append(log)
    return [ber_combine.parse(log) for log in logs]


def plot(sweeps: list[dict], out: str) -> None:
    """README-style overlay: color = SF, no titles, axes fit the data."""
    fig, ax = plt.subplots(figsize=(9.5, 6.5), constrained_layout=True)
    sfs = sorted({s["sf"] for s in sweeps})
    errs = []
    for s in sweeps:
        color = COLORS[sfs.index(s["sf"]) % len(COLORS)]
        cr = int(CR.search(s["cfg"]).group(1)) - 4
        ax.semilogy(
            s["snr"],
            np.where(s["ber"] > 0, s["ber"], np.nan),
            "o-",
            color=color,
            label=f"measured SF{s['sf']}",
        )
        step = np.diff(s["snr"]).min() if len(s["snr"]) > 1 else 1.0
        t = np.arange(s["snr"][0] - 3, s["snr"][-1] + 3 + 1e-9, step / 5)
        t_ber = theory_ber(t, s["sf"])
        ax.semilogy(
            t,
            np.where(t_ber > 0, t_ber, np.nan),
            "--",
            color=color,
            alpha=0.7,
            label=f"theory uncoded SF{s['sf']}",
        )
        t_cod = theory_ber_coded(t_ber, cr)
        ax.semilogy(
            t,
            np.where(t_cod > 0, t_cod, np.nan),
            ":",
            color=color,
            alpha=0.7,
            label=f"theory coded SF{s['sf']}",
        )
        errs.append(s["ber"][s["ber"] > 0])

    errs = np.concatenate([e for e in errs if len(e)])
    if len(errs):
        ax.set_ylim(errs.min() / 4, max(2.0, errs.max() * 2))
    # x fits the measured points too: past the last observed error there is
    # nothing to show but empty space (theory tails get cropped with it)
    lo = min(s["snr"].min() for s in sweeps)
    ends = [s["snr"][s["ber"] > 0] for s in sweeps]
    hi = max(e[-1] if len(e) else s["snr"].max() for e, s in zip(ends, sweeps))
    pad = 0.04 * (hi - lo)
    ax.set_xlim(lo - pad, hi + pad)
    ax.set_xlabel("SNR [dB]")
    ax.set_ylabel("Payload BER [1]")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)
    fig.savefig(out, dpi=150)
    print(f"wrote {out}")


def main() -> None:
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter, description=__doc__.split("\n\n")[0]
    )
    p.add_argument(
        "logs", nargs="*", help="existing ber.py sweep logs to plot instead of running sweeps"
    )
    p.add_argument("--sfs", default="7-12", help="SF list to sweep, e.g. '7,9,12' or '7-12'")
    p.add_argument("--os", type=int, default=8, help="oversampling (samples/chip)")
    p.add_argument("--snr-step", type=float, default=2, help="SNR step [dB]")
    p.add_argument(
        "--snr-min", type=float, default=None, help="override the per-SF SNR window start [dB]"
    )
    p.add_argument(
        "--snr-max", type=float, default=None, help="override the per-SF SNR window end [dB]"
    )
    p.add_argument(
        "--trials",
        default="50",
        help="noise realizations per SNR point: one number for all SFs, or "
        "'250, 9:500, 12:1000' — bare number = default, SF:N overrides one SF",
    )
    p.add_argument("--seed", type=int, default=1, help="RNG seed (all SFs)")
    p.add_argument("--cr", type=int, default=1, choices=(1, 2, 3, 4), help="coding rate 4/(4+cr)")
    p.add_argument("--pl", type=int, default=8, help="payload length in bytes")
    p.add_argument("--jobs", type=int, default=1, help="worker processes per sweep")
    p.add_argument(
        "--bin-args",
        default="",
        help="extra ber.py/binary arguments, split like a shell command line",
    )
    p.add_argument("--out", default="ber_readme.png", help="output PNG path")
    args = p.parse_args()

    if args.logs:
        sweeps = [ber_combine.parse(path) for path in args.logs]
    else:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            args.tmpdir = tmp
            sweeps = run_sweeps(args)

    plot(sweeps, args.out)


if __name__ == "__main__":
    main()
