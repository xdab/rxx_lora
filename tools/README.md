# tools/ — algorithm scratchpad

Standalone numpy/matplotlib scripts: one script, one thing, no install. Shared
kernels (upchirp, downchirp, demod LO, AWGN) live once in `common.py`.

Docstring standard: module docstring = one purpose line + references; everything
else a reader needs (parameters, pipeline internals, examples) goes in the
argparse `description`/`epilog`/per-flag `help`, so `--help` answers it.
Function docstrings 1–3 lines; inline comments only for non-obvious math. No
history, no measured-numbers war stories — git history carries those.

Plot titles: matplotlib does not wrap, it clips, so a long one goes in
`fig.suptitle` with the run parameters in a smaller `ax.set_title` subtitle.

## Scripts

One-glance index only: for a script's full behavior, flags and internals, run it
with `--help` (module docstring plus argparse).

| Script | In → out | What it does |
|--------|----------|--------------|
| `common.py` | — | Shared kernels and constants; chirp-flavor pairing lives here |
| `frame.py` | symbols/bytes → cf32 | Build a full PHY frame (preamble + 2 sync symbols + 2.25 downchirps SFD + payload, no postamble). It only modulates — the agreeing demodulator is `bin/file_lora` |
| `ber.py` | → BER curve | Monte Carlo end-to-end information BER over the shipped binary (`--cr`, PER from CRC verdicts). Table on stdout; `--verdicts` gives aligned A/B files, `--bin-args` prices the same draws under different flags; `--theory` overlays the closed-form AWGN uncoded BER and its coded estimate through the 4/(4+cr) Hamming chain |
| `mesh_rx.py` | rtl_lora → MeshPackets + node-db.json | Live Meshtastic front end: runs `bin/rtl_lora` (args forwarded), rebuilds each decoded frame's `MeshPacket` from the 16-byte on-air header parsed out of its JSONL stdout, decrypts with a channel map (`channels.json`, gitignored, built by the external `mesh_udp.py`), and prints one compact block per packet (node names resolved from a persistent `node-db.json` tracker, gitignored). `--verbose` dumps the full protobuf, `--json` emits NDJSON. Needs `meshtastic` + `cryptography`; `--replay` decodes a saved stdout capture without a radio |
| `ber_combine.py` | N logs → one plot | Overlays saved `ber.py` tables. Color separates SF/payload/file |
| `ber_readme.py` | → README-style plot | The `img/ber-vs-snr.png` generator: runs one `ber.py` sweep per SF (per-SF pre-centered SNR windows) and overlays measured (solid, markers) with the closed-form theory (uncoded dashed, coded dotted), one color per SF, no titles. Give it saved sweep logs to replot instead of sweeping |
| `regression.py` | → PASS/FAIL matrix | The commit gate: C-binary end-to-end decodes plus the real `data/capture*` frames in three netid arms. SKIPs (never FAILs) on a missing dep or unfetched LFS pointer |
| `regression_perf.py` | → RTx matrix | The gate's speed twin: same verdicts plus the real-time multiplier per line. Verdicts gate the exit code, never the speed |

Shared CLI defaults: `--sf 7` (7–12), `--bw 125000`, `--os 8` samples/chip.
IQ files are **cf32**: raw interleaved float32 I/Q, ±1.0, no header or padding.
