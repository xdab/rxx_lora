---
name: ber-sweep
description: Produce BER-vs-SNR plots and sweeps for the C receiver chain with tools/ber.py, overlay saved sweeps with ber_combine.py, A/B a receiver knob or two binaries on paired seeds, or regenerate the README BER image with ber_readme.py. Use whenever the user mentions a BER curve, sweep, waterfall, BER plot, per-SNR performance, or wants to measure/compare receiver variants — even if they just say "run a sweep" or "plot it".
---

# BER sweep

Monte Carlo end-to-end information BER vs SNR for `bin/file_lora`. The division
of labor: `tools/ber.py` runs a sweep and prints the table, saved stdout logs
are the currency of everything else, `tools/ber_combine.py` overlays logs on
one axes, `tools/ber_readme.py` generates or replots the README image. Every
flag is documented in each tool's `--help`; this skill is the workflow around
them.

## House defaults

Unless asked otherwise, sweep the Meshtastic MediumFast preset:

```sh
--sf 9 --bw 250000 --os 2 --cr 1 --netid 0x2b --center-hz 869.525e6
```

CR 4/5 is `--cr 1` (the scale is `4/(4+cr)`); `--netid` is passed through to
the binary as `-w`, hex string verbatim. `--center-hz` only feeds the crystal
error model (`--ppm`); it does not tune anything. The default window is
`--snr-min -21 --snr-max -14 --snr-step 1`. 250 trials per point is the
default depth; go lower only while probing (below), higher when asked or when
the error counts say the waterfall is still under-sampled. Sweeps
parallelize over trials with `--jobs`, which defaults to every core — pass
`--jobs $(( $(nproc) - 2 ))` to leave the machine breathing room for other
CPU-heavy work; the draws, summary and verdicts are identical either way.

MediumFast is the only preset listed here. For any other Meshtastic preset
(Short/Medium/Long × Fast/Slow/Turbo/Moderate, or the narrowband Lite/Narrow/
Tiny), see the `meshtastic-presets` skill — it has the full SF/BW/CR table
and the flag mapping — instead of guessing parameters. Its SNR window will
differ from MediumFast's; probe for it rather than reusing `-21..-14`.

Build first — `ber.py` drives the shipped `bin/file_lora` and does not build
it. Keep outputs in one tmp directory per session so logs stay reusable for
later A/Bs and replots:

```sh
make
mkdir -p /tmp/ber
python3 tools/ber.py --sf 9 --bw 250000 --os 2 --cr 1 --netid 0x2b \
    --center-hz 869.525e6 --trials 250 \
    --snr-min -21 --snr-max -14 --snr-step 1 --theory \
    --out /tmp/ber/sf9.png | tee /tmp/ber/sf9.log
```

Always `tee` the stdout table to a `.log`. Replotting a saved log is free;
re-sweeping costs wall time, and every later step (combine, A/B diff, README
replot) consumes logs, not PNGs. Keep `--theory` on unless told otherwise:
the closed-form overlay is the independent path to a "no" that tells a
plausible curve from a broken harness. If the harness itself is suspect
(receiver or ber.py just changed), run `--selftest` first: fixed trials at
regression parameters, exit 1 on mismatch.

## Probe first, then zoom

Don't open with a huge sweep. A probe run — fewer trials, wider window,
coarser step — locates the waterfall cheaply; the zoom sweep then spends the
full trials only where the curve actually bends, example:

```sh
# probe: ~1 min instead of ~10 — just enough to see where errors start
python3 tools/ber.py <preset> --trials 25 --snr-min -24 --snr-max -11 \
    --snr-step 2 --out /tmp/ber/probe.png | tee /tmp/ber/probe.log
# zoom: full trials on the region the probe found
python3 tools/ber.py <preset> --trials 250 --snr-min -20 --snr-max -15 \
    --snr-step 0.5 --out /tmp/ber/sf9.png | tee /tmp/ber/sf9.log
```

Probe trials only need to be enough to see _first_ flips, not a smooth curve.

The same probe answers "how long will the big one take": time the probe, then
scale linearly in `trials × points` — doubling trials or halving the step
doubles wall time. For a genuinely large sweep, quote the estimate before
launching it rather than discovering it an hour in.

## Sizing the run

A 0-error point is an upper bound, not a measurement, and curves stop at
their last point with an observed error. Read the error **counts** in the
table, not just the BER column: a flat zero low end means the SNR window
starts too high or trials are too few to see the first flips. Extend the
window or raise trials until the waterfall is actually observed on both ends.

## A/B: paired draws

The same `--seed` draws the same trials in both runs, so two sweeps differing
only in `--bin-args` align line for line under `--verdicts` — an A/B
attribution is then a plain `diff`:

```sh
python3 tools/ber.py <preset> ... --verdicts /tmp/ber/base.v | tee /tmp/ber/base.log
python3 tools/ber.py <preset> ... --bin-args "-x 1" \
    --verdicts /tmp/ber/knob.v | tee /tmp/ber/knob.log
diff /tmp/ber/base.v /tmp/ber/knob.v
python3 tools/ber_combine.py --by-file --out /tmp/ber/ab.png \
    /tmp/ber/base.log /tmp/ber/knob.log
```

`--by-file` is required here: both logs carry the same SF, so the default
color-by-SF key would collide. Judge the A/B on the diff before the plot —
the diff names exactly which trials flipped verdicts, while a curve can move
within noise and still look convincing. Compare configurations with identical
seed, trials, snr grid, cr, pl and netid; anything else is a confound, not a
result. This is why the probe and zoom logs land in `/tmp/ber` even when
nobody has asked for an A/B yet: the base arm of a future comparison already
exists.

## README-style plot

`tools/ber_readme.py` is the `img/ber-vs-snr.png` generator: one sweep per SF
with per-SF pre-centered SNR windows, measured solid, theory uncoded-dashed /
coded-dotted, one color per SF, no titles. It sweeps by itself, or replots
saved logs when any are passed:

```sh
python3 tools/ber_readme.py --sfs 7,8,9,10,11,12 \
    --trials 3500,10:2000,11:1500,12:1000 --snr-step 1 --os 2
python3 tools/ber_readme.py /tmp/ber/sf7.log /tmp/ber/sf9.log  # replot only
```

The first line is the recommended default for the full figure; the
`--trials` spec is `default[,SF:N]...` — 3500 per point, dialed down on the
high-SF arms.

Reach for it when the deliverable is the README figure or an all-SF
overview; reach for plain `ber.py` + `ber_combine.py` for everything else.
