#!/usr/bin/env python3
"""Monte Carlo end-to-end BER vs SNR for the C chain (bin/file_lora).

References (in-repo):
- src/receiver.c — search "RX:": the parsed coded-payload printout
- tools/frame.py — search "--hex": the binary-payload generator path
"""

import argparse
import base64
import concurrent.futures
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

import numpy as np

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import frame as frame_mod
from common import add_awgn, awgn_sigma, gauss_complex, max_jobs

_RX_RE = re.compile(r"RX: ([A-Za-z0-9+/=]*)")
_SNR_RE = re.compile(r"header: .*? snr=([-+0-9.]+) dB")
_INPUT_RE = re.compile(r"input sample (\d+)")
_STATS_RE = re.compile(r"stats: locks=(\d+)")


def bit_errs_bytes(truth: bytes, got: bytes) -> int:
    """Bit errors between TX and RX payload bytes; missing bytes cost 8."""
    errs = sum((t ^ g).bit_count() for t, g in zip(truth, got))
    return errs + 8 * max(0, len(truth) - len(got))


def rate_ratio(args) -> tuple[int, int]:
    """--file-rate as an exact up/down resampling ratio."""
    from fractions import Fraction

    return (
        Fraction(float(args.file_rate) / (args.bw * args.os))
        .limit_denominator(4096)
        .as_integer_ratio()
    )


def decimate(args, y: np.ndarray) -> np.ndarray:
    """Decimate a generated frame to the native-rate capture (--file-rate)."""
    from scipy.signal import resample_poly

    up, down = rate_ratio(args)
    return resample_poly(y, up, down)


def score_block(blk: str, truth: bytes) -> tuple[int, int, str, bool, float | None]:
    """Score one detection block's stderr against the frame's truth.

    Returns (bit errors, total bits, outcome, crc_ok, snr_est); outcome is
    "sync" when the block detected but produced no payload (sync reject or
    header-checksum failure)."""
    nbits = len(truth) * 8
    msnr = _SNR_RE.search(blk)
    snr_est = float(msnr.group(1)) if msnr else None
    mrx = _RX_RE.search(blk)
    if mrx is None:
        return nbits, nbits, "sync", False, snr_est
    got = base64.b64decode(mrx.group(1))
    crc_ok = "CRC valid!" in blk or "CRC absent" in blk
    return bit_errs_bytes(truth, got), nbits, "decode", crc_ok, snr_est


def decode_trial(
    bin_path: str, sf: int, os_f: int, args, truth, iq: str, bin_argv=()
) -> tuple[int, int, str, bool, float | None]:
    """One trial step: run bin/file_lora, score its payload bytes.

    Returns (bit errors, total bits, outcome, crc_ok, snr_est) with
    outcome one of detect/sync/decode; undetected or unsynced frames lose
    every payload bit. crc_ok is False for every non-decoded frame (no
    verdict exists). snr_est is the receiver's per-frame header SNR figure,
    None when no header was decoded.
    """
    cp = subprocess.run(
        [
            bin_path,
            "-v",
            "-s",
            str(sf),
            "-o",
            str(os_f),
            *args.netid_argv,
            *args.bin_argv,
            *bin_argv,
            iq,
        ],
        capture_output=True,
        text=True,  # a broken decoder must not crash the bench on bad UTF-8
        errors="replace",
    )
    blocks = cp.stderr.split("frame detected")[1:]
    if not blocks:
        return len(truth) * 8, len(truth) * 8, "detect", False, None
    return score_block(blocks[0], truth)


def run_trial(
    args, truth, sto: int, cfo: float, snr: float, seed: int, tmp: str
) -> tuple[int, int, str, bool, float | None]:
    """Wire one trial: generate, tail-pad, noisify, decode; score it.

    The DSP runs in-process; the binary under test stays a subprocess —
    it must be the shipped one."""
    noisy = os.path.join(tmp, "noisy.cf32")
    x, _ = frame_mod.generate(
        sf=args.sf,
        os_=args.os,
        pre=args.pre,
        sync=args.sync_syms,
        msg=truth,
        cr=args.cr,
        has_crc=True,
        sto=sto,
        cfo=cfo,
    )
    # trailing zeros keep the C output window's reads inside the file at
    # the frame end
    padded = np.concatenate([x, np.zeros((1 << args.sf) * args.os // 2, dtype=x.dtype)])
    y = add_awgn(padded, snr, np.random.default_rng(seed))
    bin_argv = []
    if getattr(args, "file_rate", None):
        # decimate to a native-rate capture; the binary resamples back up
        # itself. --bw must ride along: the binary's --bw default (250e3)
        # is not the tools' working bandwidth
        y = decimate(args, y)
        bin_argv = ["--input-rate", str(args.file_rate), "--bw", str(int(args.bw))]
    np.asarray(y, dtype=np.complex64).tofile(noisy)
    return decode_trial(args.bin, args.sf, args.os, args, truth, noisy, bin_argv)


def build_batch_file(args, snr: float, chunk: list[tuple], path: str) -> list[tuple[int, int]]:
    """Concatenate the chunk's noisy frames (with gaps) into one cf32 file.

    Each frame is built and noisified exactly as run_trial does it — its own
    padded span, its own seeded rng — so gap content cannot reach the noise
    statistics. Returns the ground-truth spans [start, end) in file samples,
    one per frame: [sto lead-in, last body sample); tail pad and gap excluded.
    """
    sps = (1 << args.sf) * args.os
    gap = args.gap_syms * sps
    spans: list[tuple[int, int]] = []
    off = 0
    with open(path, "wb") as fh:
        for i, (_, truth, sto, cfo, seed) in enumerate(chunk):
            x, _ = frame_mod.generate(
                sf=args.sf,
                os_=args.os,
                pre=args.pre,
                sync=args.sync_syms,
                msg=truth,
                cr=args.cr,
                has_crc=True,
                sto=sto,
                cfo=cfo,
            )
            padded = np.concatenate([x, np.zeros((1 << args.sf) * args.os // 2, dtype=x.dtype)])
            rng = np.random.default_rng(seed)
            y = add_awgn(padded, snr, rng)
            spans.append((off + sto, off + len(x)))
            fh.write(np.asarray(y, dtype=np.complex64).tobytes())
            off += len(y)
            if i < len(chunk) - 1 and gap:
                if args.gap == "noise":
                    # gap noise continues this frame's rng
                    g = gauss_complex(awgn_sigma(padded, snr), gap, rng)
                else:
                    g = np.zeros(gap, dtype=np.complex64)
                fh.write(g.tobytes())
                off += gap
    if getattr(args, "file_rate", None):
        # decimate the whole file once; spans ride the same exact ratio
        up, down = rate_ratio(args)
        y = np.fromfile(path, dtype=np.complex64)
        decimate(args, y).astype(np.complex64).tofile(path)
        spans = [(round(s * up / down), round(e * up / down)) for s, e in spans]
    return spans


def run_batch(
    args, snr: float, chunk: list[tuple], tmp: str
) -> tuple[list[tuple[int, int, str, bool, float | None]], int]:
    """One batch: assemble the chunk's frames into one file, one binary run,
    attribute each detection to its frame by the detect block's logged
    input-sample position. Gap/duplicate fires are false fires; frames no
    block matched lose every bit; a span hosting several attempts is scored
    by its best. Returns (per-frame results in chunk order, false fires)."""
    iq = os.path.join(tmp, "batch.cf32")
    spans = build_batch_file(args, snr, chunk, iq)
    bin_argv = []
    if getattr(args, "file_rate", None):
        bin_argv = ["--input-rate", str(args.file_rate), "--bw", str(int(args.bw))]
    cp = subprocess.run(
        [
            args.bin,
            "-v",
            "-s",
            str(args.sf),
            "-o",
            str(args.os),
            *args.netid_argv,
            *args.bin_argv,
            *bin_argv,
            iq,
        ],
        capture_output=True,
        text=True,  # a broken decoder must not crash the bench on bad UTF-8
        errors="replace",
    )
    nbits_all = [len(truth) * 8 for _, truth, _, _, _ in chunk]
    results: list[tuple[int, int, str, bool, float | None] | None] = [None for _ in nbits_all]
    false_fires = 0
    # detection blocks run from each "frame detected" line to the next; the
    # trailing stats line rides the last block and matches nothing below
    blocks = cp.stderr.split("frame detected")[1:]
    for blk in blocks:
        m = _INPUT_RE.search(blk)
        pos = int(m.group(1)) if m else -1
        idx = next((i for i, (s, e) in enumerate(spans) if s <= pos < e), None)
        if idx is None:
            false_fires += 1
            continue
        attempt = score_block(blk, chunk[idx][1])
        # a bad verdict rewinds and rescans, so one span can host several
        # attempts; the frame keeps its best (decode > sync)
        if results[idx] is None or (attempt[2] == "decode" and results[idx][2] != "decode"):
            results[idx] = attempt
        else:
            false_fires += 1
    # conservation: one detection block per lock the receiver counted
    m = _STATS_RE.search(cp.stderr)
    if m and int(m.group(1)) != len(blocks):
        print(
            f"warning: {len(blocks)} detection blocks but stats says locks={m.group(1)}",
            file=sys.stderr,
        )
    # frames no block matched: lost every payload bit
    results = [
        r if r is not None else (nb, nb, "detect", False, None) for r, nb in zip(results, nbits_all)
    ]
    return results, false_fires


def run_group_task(job) -> tuple[list[tuple[int, int, str, bool, float | None]], int]:
    """Pool worker: one binary-run unit, in a temp dir of its own. A group is
    fully described by the draws main() already made, so fan-out cannot move
    a result."""
    args, snr, chunk = job
    with tempfile.TemporaryDirectory() as tmp:
        if args.batch == 1:
            _, truth, sto, cfo, seed = chunk[0]
            return [run_trial(args, truth, sto, cfo, snr, seed, tmp)], 0
        return run_batch(args, snr, chunk, tmp)


def selftest(args) -> bool:
    """Fixed trials at the regression parameters: 0 bit errors, CRC valid —
    one on the single-trial path, one batched (attribution under test)."""
    with tempfile.TemporaryDirectory() as tmp:
        e, _, outcome, crc, _ = run_trial(args, b"Hello rtl_lora", 300, -4.7, 10, 1, tmp)
    ok = outcome == "decode" and e == 0 and crc
    print(
        f"selftest: {'PASS' if ok else 'FAIL'}  "
        f"(sto=300 cfo=-4.7 snr=10: outcome={outcome}, {e} bit errs, "
        f"crc={'ok' if crc else 'BAD'})"
    )
    bargs = argparse.Namespace(**vars(args))
    bargs.batch = 4
    bargs.gap_syms = 8
    bargs.gap = "zeros"
    chunk = [
        (ti, bytes([0x41 + ti]) * args.pl, sto, -4.7, 100 + ti)
        for ti, sto in enumerate((0, 50, 120, 250))
    ]
    with tempfile.TemporaryDirectory() as tmp:
        results, ff = run_batch(bargs, 10, chunk, tmp)
    bok = ff == 0 and all(o == "decode" and e == 0 and c for e, _, o, c, _ in results)
    print(
        f"selftest batch: {'PASS' if bok else 'FAIL'}  "
        f"(4 frames, one run, stos 0/50/120/250: outcomes="
        f"{[o for _, _, o, _, _ in results]}, {ff} false fires)"
    )
    return ok and bok


def main() -> None:
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "Per SNR point, how many payload BITS the compiled receiver gets "
            "wrong. Each trial generates a "
            "frame (frame.generate), pads it with sps//2 trailing zeros so the "
            "C output window stays in bounds, noises it (common.add_awgn), "
            "then hands the file to the shipped binary — exactly as a human "
            "would.\n"
            "The score is end-to-end information BER over the dewhitened "
            "payload bytes, read byte-exactly from the receiver's 'RX:' line "
            "with the CRC verdict beside it (PER = lost frames); a lost frame "
            "counts every payload bit wrong.\n"
            "Zero-power leading zeros (--sto) dilute awgn's file-average power "
            "estimate by up to ~0.3 dB — an accepted bias."
        ),
    )
    p.add_argument("--sf", type=int, default=7, help="spreading factor (7..12)")
    p.add_argument("--os", type=int, default=2, help="oversampling (samples/chip)")
    p.add_argument(
        "--bw",
        type=float,
        default=125e3,
        help="bandwidth in Hz (file-mode time scaling: fs = bw*os; "
        "BER vs per-sample SNR is BW-invariant)",
    )
    p.add_argument("--pre", type=int, default=8, help="preamble upchirps (6..65535)")
    p.add_argument("--snr-min", type=float, default=-12, help="first SNR point [dB]")
    p.add_argument("--snr-max", type=float, default=8, help="last SNR point [dB]")
    p.add_argument("--snr-step", type=float, default=2, help="SNR step [dB]")
    p.add_argument("--trials", type=int, default=50, help="noise realizations per SNR point")
    p.add_argument("--seed", type=int, default=1, help="RNG seed")
    p.add_argument(
        "--jobs",
        type=int,
        default=max_jobs(),
        help="worker processes for the trial matrix (1 = serial; the draws, "
        "the summary and the --verdicts lines are identical either way)",
    )
    p.add_argument(
        "--sto-max",
        type=int,
        default=None,
        dest="sto_max",
        help="max STO in samples (default: one symbol window)",
    )
    p.add_argument(
        "--cfo-max",
        type=float,
        default=None,
        dest="cfo_max",
        help="max |CFO| in FFT bins, overriding the crystal model below",
    )
    p.add_argument(
        "--ppm",
        type=float,
        default=20.0,
        help="crystal error in ppm, drawn uniformly in +-this: a typical "
        "dongle's error, not its +-36 ppm tolerance (drawing that puts a "
        "share of frames past the +-BW/4 acquisition limit and manufactures "
        "high-SNR errors)",
    )
    p.add_argument(
        "--center-hz",
        type=float,
        default=868e6,
        dest="center_hz",
        help="carrier the ppm figure refers to; the product is what matters",
    )
    p.add_argument(
        "--cr",
        type=int,
        default=1,
        choices=(1, 2, 3, 4),
        help="coding rate 4/(4+cr): explicit header, CRC on. End-to-end "
        "information BER over the dewhitened payload bytes",
    )
    p.add_argument("--pl", type=int, default=8, dest="pl", help="payload length in bytes (2..255)")
    p.add_argument(
        "--netid",
        default="0x12",
        help="sync word passed to the binary as -w: the netid these frames "
        "carry (sync=[8,16]). The netid check is where the receiver calibrates "
        "its payload grid (sync.c net_id_off); 0 = no check, no calibration, "
        "and the coarse sync's chip error back in the curve",
    )
    p.add_argument(
        "--bin",
        default=os.path.join(ROOT, "bin", "file_lora"),
        help="path to the C binary under test",
    )
    p.add_argument(
        "--bin-args",
        default="",
        metavar="ARGS",
        help="extra arguments for the binary, split like a shell command line. "
        "The draws are unaffected, so two runs that differ only in flags "
        "still align line for line under --verdicts: a knob, A/B'd",
    )
    p.add_argument(
        "--verdicts",
        default=None,
        metavar="PATH",
        help="write one line per trial (inputs + verdict + the receiver's "
        "header SNR estimate), so a two-binary A/B attribution is a plain "
        "`diff` of two files: the same --seed draws the same trials in "
        "both runs, so the lines align and only the verdicts differ",
    )
    p.add_argument(
        "--file-rate",
        type=float,
        default=None,
        help="decimate the generated frame to this file rate first; the "
        "binary is handed --input-rate and resamples it back up itself",
    )
    p.add_argument(
        "--batch",
        type=int,
        default=1,
        help="frames concatenated (with gaps) into ONE binary run, scored "
        "by attributing each detection's logged input-sample position to "
        "its ground-truth frame; 1 = one launch per trial. Noise draws are "
        "unchanged, but a mid-stream frame's detection context is the gap "
        "behind it, not a file start — A/B against batch=1 on the same "
        "--seed compares per-point statistics, not verdict lines",
    )
    p.add_argument(
        "--gap",
        choices=("noise", "zeros"),
        default="zeros",
        help="inter-frame gap content for batched runs. zeros (default): "
        "the gap can never fire the detector; noise: real-noise history, "
        "the arm for measuring DETECT false alarms — at the cost of a "
        "small frame-loss floor from gap fires",
    )
    p.add_argument(
        "--gap-syms",
        type=int,
        default=8,
        dest="gap_syms",
        help="inter-frame gap in symbol windows for batched runs; must "
        "exceed the receiver's rewind depth past a bad fire",
    )
    p.add_argument("--out", default="ber.png", help="output PNG path")
    p.add_argument(
        "--theory",
        action="store_true",
        help="overlay the closed-form uncoded AWGN theory BER (Elshabrawy & "
        "Robert 2018, arXiv:1905.11252) and its "
        "coded estimate through the payload's 4/(4+cr) Hamming code on "
        "iid-after-deinterleaving bits (doc #18): drawn at 5x the --snr-step "
        "resolution, spanning +-3 dB past the swept ends, while the axes stay "
        "cropped to the measured points",
    )
    p.add_argument(
        "--selftest",
        action="store_true",
        help="fixed trial(s) at the regression parameters, exit 1 on mismatch",
    )
    args = p.parse_args()

    if not os.access(args.bin, os.X_OK):
        p.error(f"{args.bin} missing or not executable (run make)")
    n = 1 << args.sf
    sps = n * args.os
    if not 2 <= args.pl <= 255:
        p.error(f"--pl {args.pl} out of range 2..255 (8-bit header, CRC on)")
    if args.batch < 1:
        p.error(f"--batch {args.batch} must be >= 1")
    if args.gap_syms < 0:
        p.error(f"--gap-syms {args.gap_syms} must be >= 0")
    sto_max = args.sto_max if args.sto_max is not None else sps
    args.netid_argv = ["-w", args.netid]
    # the frames must carry the netid the binary filters by: the sync symbols
    # are the word's nibbles shifted up (gr-lora_sdr nibble<<3, 0x12 -> 8,16);
    # netid 0 = no filter, keep the private 0x12 on air
    nid = int(args.netid, 0)
    args.sync_syms = [8, 16] if nid == 0 else [(nid >> 4) << 3, (nid & 0xF) << 3]
    args.bin_argv = shlex.split(args.bin_args)

    # CFO is a property of the crystal, not of the spreading factor: model
    # it in ppm at a carrier and convert per SF (acquisition limit: +-BW/4)
    cfo_hz_max = args.center_hz * args.ppm * 1e-6
    if args.cfo_max is None:
        args.cfo_max = cfo_hz_max / (args.bw / n)  # Hz -> bins at this SF/BW
    args.cfo_hz_max = cfo_hz_max

    if args.selftest:
        sys.exit(0 if selftest(args) else 1)

    snrs = np.arange(args.snr_min, args.snr_max + 1e-9, args.snr_step)
    rng = np.random.default_rng(args.seed)
    cfg = f"coded 4/{args.cr + 4}, {args.pl} B payload"
    print(
        f"SF={args.sf}  os={args.os}  pre={args.pre}  bw={args.bw:g}  {cfg}  "
        f"{args.trials} trials/point  (STO in [0, {sto_max}), "
        f"CFO in +-{args.cfo_max:.4g} bins = +-{args.cfo_hz_max / 1e3:.4g} kHz "
        f"(+-{args.ppm:g} ppm at {args.center_hz / 1e6:g} MHz), netid {args.netid})",
        flush=True,
    )
    print(" SNR [dB]       BER     bit errs   detect  sync  ok  crc", flush=True)
    # Draw the whole matrix up front, in serial-loop order: the --verdicts
    # draw sequence is part of the A/B contract and must not move.
    draws = []
    for snr in snrs:
        for ti in range(args.trials):
            truth = rng.integers(0, 256, args.pl, dtype=np.uint8).tobytes()
            sto = int(rng.integers(0, sto_max + 1))
            cfo = float(rng.uniform(-args.cfo_max, args.cfo_max))
            awgn_seed = int(rng.integers(1 << 31))
            draws.append((snr, ti, truth, sto, cfo, awgn_seed))

    # --batch draws per binary-run unit, never across SNR points; order is
    # preserved, so the --verdicts draw sequence does not move
    groups: list[tuple[float, list[tuple]]] = []
    cur: list[tuple] = []
    cur_snr = None
    for snr, ti, truth, sto, cfo, awgn_seed in draws:
        if cur and (snr != cur_snr or len(cur) == args.batch):
            groups.append((cur_snr, cur))
            cur = []
        cur.append((ti, truth, sto, cfo, awgn_seed))
        cur_snr = snr
    if cur:
        groups.append((cur_snr, cur))
    jobs = [(args, snr, chunk) for snr, chunk in groups]

    bers = []
    verdicts = []
    false_fires = 0
    by_snr = {
        float(s): {"errs": 0, "bits": 0, "fails": {"detect": 0, "sync": 0}, "ok": 0, "crc_ok": 0}
        for s in snrs
    }
    # progress to stderr; stdout stays exactly the final table (ber_combine
    # parses these logs)
    tick = 100
    t0 = time.perf_counter()
    done = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as ex:
        for (snr, chunk), (results, ff) in zip(groups, ex.map(run_group_task, jobs)):
            false_fires += ff
            acc = by_snr[float(snr)]
            for (ti, _, sto, cfo, awgn_seed), (e, nb, outcome, crc, est) in zip(chunk, results):
                acc["errs"] += e
                acc["bits"] += nb
                if outcome in acc["fails"]:
                    acc["fails"][outcome] += 1
                else:
                    acc["ok"] += 1
                    acc["crc_ok"] += crc
                verdicts.append(
                    f"snr={snr:g} t={ti} seed={awgn_seed} sto={sto} cfo={cfo:.3f} "
                    f"outcome={outcome} errs={e}/{nb} "
                    f"crc={'ok' if crc else 'bad'} "
                    f"est={'-' if est is None else f'{est:+.1f}'}"
                )
                done += 1
                if done % tick and ti != args.trials - 1:
                    continue
                el = time.perf_counter() - t0
                print(
                    f"  [{el:6.1f} s] {done:6d}/{len(draws)} trials   {done / el:6.1f} trials/s",
                    file=sys.stderr,
                    flush=True,
                )
                if ti != args.trials - 1:  # point still running: no row yet
                    continue
                bers.append(acc["errs"] / acc["bits"])
                print(
                    f"{snr:7.1f}   {bers[-1]:.3e}   {acc['errs']:6d}/{acc['bits']}   "
                    f"{acc['fails']['detect']:5d}  {acc['fails']['sync']:4d}  "
                    f"{acc['ok']:4d}  {acc['crc_ok']:4d}",
                    flush=True,
                )
    if false_fires:
        print(f"false fires: {false_fires}", file=sys.stderr)
    if args.verdicts:
        with open(args.verdicts, "w") as vh:
            vh.write("\n".join(verdicts) + "\n")

    plot_ber(args, snrs, bers, cfg)


def theory_ber(snr_db, sf: int):
    """Closed-form uncoded AWGN BER on the tools/' per-complex-sample SNR:
    SER eq. (18) of arXiv:1905.11252, scaled to bits by 2^(SF-1)/(2^SF-1)
    (arXiv:1911.10245 eq. 10). Unit-modulus chirps put Es/N0 = snr * 2^SF."""
    n = 1 << sf
    gamma_em = 0.57722  # Euler-Mascheroni constant
    arg = (
        np.sqrt(2 * 10 ** (np.asarray(snr_db) / 10) * n) - np.sqrt(2 * (np.log(2) * sf + gamma_em))
    ) / np.sqrt(2)
    return n / (2 * (n - 1)) * np.array([0.5 * math.erfc(a) for a in arg])


def theory_ber_coded(pb, cr: int):
    """Coded payload BER estimate: the 4/(4+cr) Hamming code on iid bits
    (arXiv:1911.10245). Enumerates every received word, decodes it to its
    nearest codeword(s) — what the exhaustive ML in src/hamming.c does for
    iid symmetric errors — and averages the expected nibble bit errors."""
    n = cr + 4
    code = [frame_mod.hamming_enc_nibble(k, cr) for k in range(16)]
    nib_err = [k.bit_count() for k in range(16)]
    words = np.arange(1 << n)
    wt = np.array([int(w).bit_count() for w in words])
    dist = np.array([[int(w ^ c).bit_count() for c in code] for w in words])
    near = dist == dist.min(axis=1, keepdims=True)
    errs = (near * np.array(nib_err)).sum(axis=1) / near.sum(axis=1)
    pb = np.asarray(pb, dtype=float)[:, None]
    return (pb**wt * (1 - pb) ** (n - wt) * errs).sum(axis=1) / 4


def plot_ber(args, snrs, bers, cfg: str) -> None:
    """BER curve, written to args.out (matplotlib imported lazily: never
    paid by --selftest or a crashed sweep)."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(9, 6), constrained_layout=True)
    # a log axis cannot show zero BER; NaN breaks the line there instead
    ax.semilogy(
        snrs,
        np.where(np.asarray(bers) > 0, bers, np.nan),
        "o-",
        label="C chain end to end (0 errors: no point)",
    )
    ax.set_xlim(float(snrs[0]), float(snrs[-1]))
    ylim = ax.get_ylim()
    if args.theory:
        t_snrs = np.arange(args.snr_min - 3, args.snr_max + 3 + 1e-9, args.snr_step / 5)
        t_ber = theory_ber(t_snrs, args.sf)
        ax.semilogy(
            t_snrs,
            np.where(t_ber > 0, t_ber, np.nan),
            "k--",
            label=f"theory AWGN uncoded (SF={args.sf})",
        )
        t_coded = theory_ber_coded(t_ber, args.cr)
        ax.semilogy(
            t_snrs,
            np.where(t_coded > 0, t_coded, np.nan),
            "k:",
            label=f"theory AWGN coded 4/{args.cr + 4} (Hamming)",
        )
        ax.set_ylim(*ylim)
    ax.set_xlabel("SNR per complex sample [dB]")
    ax.set_ylabel("payload bit error rate")
    fig.suptitle(f"bin/file_lora end-to-end BER over AWGN (SF={args.sf})", fontsize=13)
    ax.set_title(
        f"os={args.os}, pre={args.pre}, {cfg}, {args.trials} trials/point, seed={args.seed}",
        fontsize=9,
    )
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
