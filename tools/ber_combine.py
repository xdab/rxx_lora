#!/usr/bin/env python3
"""Overlay several ber.py sweeps on one BER-vs-SNR plot."""

import argparse
import os
import re
import sys

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ber.py's per-point row: snr, ber, bit-errs, detect, sync, ok, crc
ROW = re.compile(r"^\s*(-?[\d.]+)\s+([\d.e+-]+)\s+(\d+)/(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")
# ber.py's header line: SF=7  os=8  pre=8  bw=250000  coded 4/5, 8 B payload  ...
HEAD = re.compile(
    r"SF=(\d+)\s+os=(\d+)\s+pre=(\d+)(?:\s+bw=(\d+))?\s+"
    r"(coded [^\n]*?)\s+(\d+) trials/point"
)
# the header carries the payload length: "coded 4/5, 8 B payload, ..."
PL = re.compile(r"(\d+)\s*B payload")
COLORS = ["tab:blue", "tab:orange", "tab:green", "tab:red", "tab:purple", "tab:brown"]


def parse(path):
    """One sweep log -> header dict + per-point arrays."""
    head = None
    rows = []
    with open(path) as fh:
        for line in fh:
            if head is None:
                m = HEAD.search(line)
                if m:
                    head = m.groups()
                continue
            m = ROW.match(line)
            if m:
                rows.append([float(x) for x in m.groups()])
    if head is None:
        raise SystemExit(f"{path}: no ber.py header line (is this a sweep log?)")
    if not rows:
        raise SystemExit(f"{path}: no data rows")
    sf, os_, pre, bw, cfg, trials = head
    a = np.array(rows)
    return {
        "sf": int(sf),
        "os": int(os_),
        "pre": int(pre),
        "bw": int(bw) if bw else None,
        "cfg": cfg.strip(),
        "pl": int(m.group(1)) if (m := PL.search(cfg)) else None,
        "trials": int(trials),
        "snr": a[:, 0],
        "ber": a[:, 1],
        "lost": a[:, 4] + a[:, 5],
    }


def curve(ax, d, color, label):
    """One sweep; NaN out the zero-error points, ring the frame-loss ones."""
    zero = d["ber"] <= 0
    y = np.where(zero, np.nan, d["ber"])
    lost = (d["lost"] > 0) & ~zero
    ax.semilogy(d["snr"], y, "--", color=color, marker="s", label=label)
    if lost.any():
        ax.semilogy(
            d["snr"][lost],
            d["ber"][lost],
            "s",
            color=color,
            markerfacecolor="none",
            markersize=11,
            markeredgewidth=1.5,
        )
    return d["ber"][~zero]


def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "Reads the stdout tables of N saved ber.py sweeps and draws them "
            "together, one curve each: color = SF. Curves stop at their last "
            "point with an observed error — a 0/total BER is an upper bound, "
            "not a measurement.\n"
            "examples:\n"
            "  python3 tools/ber.py --sf 7 --trials 200 --out cod7.png | tee cod7.log\n"
            "  python3 tools/ber_combine.py --out combined.png cod7.log cod9.log"
        ),
    )
    p.add_argument("logs", nargs="+", help="ber.py sweep logs (stdout, tee'd or >)")
    p.add_argument(
        "--color-by",
        choices=["sf", "pl"],
        default="sf",
        help="what the curve color encodes: spreading factor, or payload length in bytes",
    )
    p.add_argument(
        "--by-file",
        action="store_true",
        help="color and label each curve by its log file instead of by SF — for "
        "before/after of one configuration, where the SF key would collide",
    )
    p.add_argument("--out", default="ber_combined.png", help="output PNG path")
    args = p.parse_args()

    sweeps = [parse(path) for path in args.logs]
    common = ["os", "pre", "bw", "trials"]
    for key in common:
        vals = {s[key] for s in sweeps}
        if len(vals) > 1:
            print(
                f"warning: sweeps differ in {key} ({sorted(map(str, vals))}) — "
                "curves may not be comparable",
                file=sys.stderr,
            )

    fig, ax = plt.subplots(figsize=(9.5, 6.5), constrained_layout=True)
    # the color key: SF, or coded payload length in bytes
    key = (lambda d: d["sf"]) if args.color_by == "sf" else (lambda d: d["pl"])
    keys = sorted({key(d) for d in sweeps})
    lows, ends = [], []
    for i, (d, path) in enumerate(zip(sweeps, args.logs)):
        color = (
            COLORS[i % len(COLORS)] if args.by_file else COLORS[keys.index(key(d)) % len(COLORS)]
        )
        cfg = d["cfg"].replace(",", "").strip()
        tag = f"{os.path.splitext(os.path.basename(path))[0]}: " if args.by_file else ""
        lows.append(curve(ax, d, color, f"{tag}SF{d['sf']} {cfg}"))
        ends.append(d["snr"][d["ber"] > 0])

    d0 = sweeps[0]
    params = f"os={d0['os']}, pre={d0['pre']}, {d0['trials']} trials/point"
    if d0["bw"]:
        params = f"BW {d0['bw'] / 1e3:g} kHz, " + params
    ax.set_xlabel("SNR per complex sample [dB]")
    ax.set_ylabel("payload bit error rate")
    ax.set_title(
        f"bin/file_lora end-to-end BER over AWGN — {params}\n"
        f"color = {'log file' if args.by_file else ('SF' if args.color_by == 'sf' else 'payload bytes')}; "
        "open markers = SNR points with frames lost in DETECT/SYNC; "
        "curves stop at the last point with an observed error",
        fontsize=10,
    )
    # axes fit the plotted points, not the swept range: past the last
    # observed error there is nothing to show but empty space
    lo = min(s["snr"].min() for s in sweeps)
    # a sweep with no errors anywhere contributes no drawn point: fall back
    # to its full swept range so the limits stay finite
    hi = max((e[-1] if len(e) else s["snr"].max()) for e, s in zip(ends, sweeps))
    pad = 0.04 * (hi - lo)
    ax.set_xlim(lo - pad, hi + pad)
    drawn = [l for l in lows if len(l)]
    ax.set_ylim(min(l.min() for l in drawn) / 4 if drawn else 1e-4, 3)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=9, loc="lower left")
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
