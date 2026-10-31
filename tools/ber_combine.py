#!/usr/bin/env python3
"""Overlay several ber.py / data/sweep.py sweeps on one BER- or PER-vs-SNR plot."""

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
# data/sweep.py's per-point row: snr, trials, detect, sync, crc-bad, ok, PER, BER
# (field 2 is an integer trial count where ber.py's is a float BER, and no
# "errs/nbits" slash — the two row shapes cannot be confused)
SWEEP_ROW = re.compile(
    r"^\s*(-?[\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.e+-]+)\s*$"
)
# header line, ber.py ("coded 4/5, 8 B payload ...") or data/sweep.py
# ("cr=4/5 pl=8 netid=0x12 ..."): SF, os, pre, optional bw, free config text,
# trials/point
HEAD = re.compile(
    r"SF=(\d+)\s+os=(\d+)\s+pre=(\d+)(?:\s+bw=(\d+))?\s+"
    r"((?:coded )?[^\n]*?)\s+(\d+) trials/point"
)
# the payload length: ber.py's "8 B payload" or sweep.py's "pl=8"
PL = re.compile(r"(?:(\d+)\s*B payload)|pl=(\d+)")
COLORS = ["tab:blue", "tab:orange", "tab:green", "tab:red", "tab:purple", "tab:brown"]


def parse(path):
    """One sweep log -> header dict + per-point arrays."""
    head = None
    ber_rows = []  # ber.py: snr, ber, errs, nbits, detect, sync, ok, crc
    sweep_rows = []  # sweep.py: snr, trials, detect, sync, crc-bad, ok, PER, BER
    with open(path) as fh:
        for line in fh:
            if head is None:
                m = HEAD.search(line)
                if m:
                    head = m.groups()
                continue
            if ROW.match(line):
                ber_rows.append([float(x) for x in ROW.match(line).groups()])
            elif SWEEP_ROW.match(line):
                sweep_rows.append([float(x) for x in SWEEP_ROW.match(line).groups()])
    if head is None:
        raise SystemExit(f"{path}: no ber.py header line (is this a sweep log?)")
    if not ber_rows and not sweep_rows:
        raise SystemExit(f"{path}: no data rows")
    if ber_rows and sweep_rows:
        raise SystemExit(f"{path}: mixed ber.py and sweep.py rows")
    sf, os_, pre, bw, cfg, trials = head
    plm = PL.search(cfg)
    d = {
        "sf": int(sf),
        "os": int(os_),
        "pre": int(pre),
        "bw": int(bw) if bw else None,
        "cfg": cfg.strip(),
        "pl": int(plm.group(1) or plm.group(2)) if plm else None,
        "trials": int(trials),
    }
    if ber_rows:
        a = np.array(ber_rows)
        d["snr"], d["ber"], d["per"], d["lost"] = a[:, 0], a[:, 1], None, a[:, 4] + a[:, 5]
    else:
        a = np.array(sweep_rows)
        d["snr"], d["ber"], d["per"], d["lost"] = a[:, 0], a[:, 7], a[:, 6], a[:, 2] + a[:, 3]
    return d


def curve(ax, d, color, label, metric, lw=None):
    """One sweep; NaN out the zero-error points, ring the frame-loss ones."""
    y_all = d[metric]
    zero = y_all <= 0
    y = np.where(zero, np.nan, y_all)
    lost = (d["lost"] > 0) & ~zero
    ax.semilogy(d["snr"], y, "--", color=color, marker="s", label=label, linewidth=lw)
    if lost.any():
        ax.semilogy(
            d["snr"][lost],
            y_all[lost],
            "s",
            color=color,
            markerfacecolor="none",
            markersize=11,
            markeredgewidth=1.5 if lw is None else 1.0,
        )
    return y_all[~zero]


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
    p.add_argument(
        "--per",
        action="store_true",
        help="plot packet error rate instead of BER (sweep.py logs only; "
        "ber.py logs carry no PER column)",
    )
    p.add_argument("--out", default="ber_combined.png", help="output PNG path")
    args = p.parse_args()

    sweeps = [parse(path) for path in args.logs]
    metric = "per" if args.per else "ber"
    if args.per and any(s["per"] is None for s in sweeps):
        p.error("--per needs sweep.py logs; ber.py tables have no PER column")
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
    # many curves: a sequential colormap (distinct colors, thin lines) instead
    # of the six-color qualitative set, which would repeat and blur together
    many = len(sweeps) > len(COLORS)
    colors = plt.cm.viridis(np.linspace(0, 0.95, len(sweeps))) if many else COLORS
    lw = 1.0 if many else None
    # the color key: SF, or coded payload length in bytes
    key = (lambda d: d["sf"]) if args.color_by == "sf" else (lambda d: d["pl"])
    keys = sorted({key(d) for d in sweeps})
    lows, ends = [], []
    for i, (d, path) in enumerate(zip(sweeps, args.logs)):
        color = colors[i] if (args.by_file or many) else colors[keys.index(key(d)) % len(colors)]
        cfg = d["cfg"].replace(",", "").strip()
        tag = f"{os.path.splitext(os.path.basename(path))[0]}: " if args.by_file else ""
        lows.append(curve(ax, d, color, f"{tag}SF{d['sf']} {cfg}", metric, lw))
        ends.append(d["snr"][d[metric] > 0])

    d0 = sweeps[0]
    params = f"os={d0['os']}, pre={d0['pre']}, {d0['trials']} trials/point"
    if d0["bw"]:
        params = f"BW {d0['bw'] / 1e3:g} kHz, " + params
    ax.set_xlabel("SNR per complex sample [dB]")
    ax.set_ylabel("payload packet error rate" if args.per else "payload bit error rate")
    ax.set_title(
        f"bin/file_lora end-to-end {'PER' if args.per else 'BER'} over AWGN — {params}\n"
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
    top = 1.1 if args.per else 3
    ax.set_ylim(min(l.min() for l in drawn) / 4 if drawn else 1e-4, top)
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=9, loc="lower left")
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
