#!/usr/bin/env python3
"""regression.py — the rtl_lora selftest matrix, the commit gate.

Cases end in bin/file_lora; this script only builds their IQ and scores it.
"""

import argparse
import base64
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from functools import partial

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
FRAME = os.path.join(TOOLS, "frame.py")
sys.path.insert(0, TOOLS)

S_BIN = "End to end; clean"
S_NOISE = "End to end; under AWGN"
S_CAP = "Real captured frame (data/capture*)"
S_MULTI = "Combined frame captures (data/capture*)"

NOISE_AVG = 1  # the detector needs shallow averaging in AWGN (see noise_case)
# dB, full-band per complex sample; every draw at every point must decode
NOISE_SNR = {7: [-8.0, -4.0, 0.0], 9: [-12.0, -6.0, 0.0], 12: [-20.0, -10.0, 0.0]}
NOISE_DRAWS = 16  # AWGN realizations per SNR point

CAPTURE_CONJ = {"capture1": True, "capture2": True, "capture3": False}  # mirrored spectrum

# --center-freq enables sync.c's CFO->SFO inference, which long frames need
CAPTURE_CENTER_HZ = 869.525e6
CAPTURE_BW_HZ = 250e3
CAPTURE_FILE_RATE_HZ = 1024e3  # the binary resamples to bw*os

CAP_FRAMES = {"capture1": 9, "capture2": 21, "capture3": 49}

LADDER = (255, 144, 72, 36, 8)  # pay_len rungs; 255 = protocol max
LADDER_SF = (7, 9, 12)  # SFs the full ladder covers


class CaseFail(Exception):
    def __init__(self, cmd, out):
        super().__init__(cmd)
        self.cmd = cmd
        self.out = out


class CaseSkip(Exception):
    """Skip, not fail: missing dependency or unfetched LFS object."""


class Process:
    """A subprocess: argv() builds the command, run() spawns it."""

    def argv(self):
        raise NotImplementedError

    def run(self, verdict=False):
        """`verdict` allows a non-zero exit that is data, not failure."""
        argv = self.argv()
        cp = subprocess.run(argv, capture_output=True, text=True, errors="replace")
        if cp.returncode != 0 and not verdict:
            raise CaseFail(" ".join(argv), (cp.stdout + cp.stderr).strip()[-1200:])
        return cp


@dataclass
class Frame(Process):
    """tools/frame.py: one generated frame on regression's clean channel."""

    iq: str
    sf: int = 7
    cr: int | None = None
    pad: int = 7777
    sto: int = 300
    cfo: float = -4.7
    os_factor: int = 2
    no_crc: bool = False
    msg: str | None = None
    payload_hex: str | None = None

    def argv(self):
        argv = [
            sys.executable,
            FRAME,
            "--iq",
            self.iq,
            "--sf",
            str(self.sf),
            "--os",
            str(self.os_factor),
        ]
        if self.pad:
            argv += ["--pad", str(self.pad)]
        if self.sto:
            argv += ["--sto", str(self.sto)]
        if self.cfo:
            argv += ["--cfo", str(self.cfo)]
        if self.cr is not None:
            argv += ["--cr", str(self.cr)]
        if self.no_crc:
            argv.append("--no-crc")
        if self.payload_hex is not None:
            argv += ["--hex", self.payload_hex]
        elif self.msg is not None:
            argv += ["--msg", self.msg]
        return argv


@dataclass
class Receiver(Process):
    """One bin/file_lora run: typed options in, the matching CLI flags out."""

    bin_path: str
    iq: str
    sf: int
    os_factor: int | None = None
    avg: int | None = None
    center_freq: float | None = None
    bw: float | None = None
    input_rate: float | None = None
    conj: bool = False
    netid: str | None = None

    def argv(self):
        argv = [self.bin_path, "-s", str(self.sf)]
        if self.os_factor is not None:
            argv += ["-o", str(self.os_factor)]
        if self.avg is not None:
            argv += ["-a", str(self.avg)]
        if self.center_freq is not None:
            argv += ["--center-freq", str(self.center_freq)]
        if self.bw is not None:
            argv += ["--bw", str(self.bw)]
        if self.conj:
            argv.append("--conj")
        if self.netid:
            argv += ["-w", self.netid]
        if self.input_rate is not None:
            argv += ["--input-rate", str(self.input_rate)]
        argv.append(self.iq)
        return argv


def case_rng(seed, label):
    """Stable per-label RNG: reordering the matrix shifts no payload."""
    digest = hashlib.blake2b(f"{seed}:{label}".encode(), digest_size=8).digest()
    return random.Random(int.from_bytes(digest, "big"))


def rand_bytes(rng, n=None):
    """n random octets (default 2..16; a CRC'd frame needs at least 2)."""
    return bytes(rng.randrange(256) for _ in range(n or rng.randint(2, 16)))


def hex_short(b: bytes, head=16):
    """Hex for a failure line, elided past `head` bytes."""
    s = b.hex() if b is not None else None
    if s is None or len(b) <= head:
        return s or "-"
    return f"{b[:head].hex()}..+{len(b) - head}B"


def diff_note(truth, got):
    """(n good, first bad index) for a payload mismatch."""
    n = 0
    for a, b in zip(truth, got or b""):
        if a != b:
            return n, n
        n += 1
    return n, None if len(got or b"") == len(truth) else n


def bin_case(iq, bin_path, sf, truth, want_crc=True, **frame_args):
    """Generate a frame, decode it, score it; `frame_args` are Frame fields."""
    Frame(iq, sf=sf, **frame_args).run()
    return score_bin(Receiver(bin_path=bin_path, iq=iq, sf=sf), truth, want_crc)


def parse_frames(stdout):
    """The JSONL frame records rtl_lora wrote to stdout."""
    return [json.loads(line) for line in stdout.splitlines() if line.strip()]


def score_bin(rx, truth, want_crc=True):
    return check_out(rx.run(verdict=True), rx.argv(), truth, want_crc)


def check_out(cp, cmd, truth, want_crc=True):
    """Score one run's stdout frame records against `truth`."""
    recs = parse_frames(cp.stdout)
    if not recs:
        raise CaseFail(" ".join(cmd), (cp.stdout + cp.stderr).strip()[-1200:])
    rec = recs[0]
    got = base64.b64decode(rec["packet"])
    crc = rec["crc_valid"] is True if want_crc else rec["crc_present"] is False
    if got != truth or not crc:
        n_good, first_bad = diff_note(truth, got)
        where = "" if first_bad is None else f"; diverges at byte {first_bad}"
        raise CaseFail(
            " ".join(cmd),
            f"expected {len(truth)} B {hex_short(truth)} + CRC "
            f"{'valid' if want_crc else 'absent'}; got "
            f"{len(got) if got is not None else 0} B {hex_short(got)}, "
            f"{n_good} good{where}, CRC "
            f"{'valid' if crc else 'INVALID/absent'}",
        )
    return f"{len(truth)} B exact, CRC {'valid' if want_crc else 'absent'}"


def build_cases(seed, bin_path, per_frame=False):
    """The matrix: (section, label, fn(iq) -> note)."""
    cases = []

    # 1. generated frames through the C binary, SF 7..12
    for sf in range(7, 13):
        label = f"bin sf{sf} coded bytes"
        pay = rand_bytes(case_rng(seed, label))
        cases.append(
            (
                S_BIN,
                label,
                partial(bin_case, bin_path=bin_path, sf=sf, truth=pay, cr=1, payload_hex=pay.hex()),
            )
        )
    # 1b. payload ladder: long payloads walk the SFO compensation
    for sf in LADDER_SF:
        for n in LADDER:
            label = f"bin sf{sf} len{n}"
            pay = rand_bytes(case_rng(seed, label), n)
            cases.append(
                (
                    S_BIN,
                    label,
                    partial(
                        bin_case, bin_path=bin_path, sf=sf, truth=pay, cr=1, payload_hex=pay.hex()
                    ),
                )
            )
    label = "bin sf7 len1 no-crc"
    pay = rand_bytes(case_rng(seed, label), 1)
    cases.append(
        (
            S_BIN,
            label,
            partial(
                bin_case,
                bin_path=bin_path,
                sf=7,
                truth=pay,
                want_crc=False,
                cr=1,
                no_crc=True,
                payload_hex=pay.hex(),
            ),
        )
    )
    label = "bin sf7 hello crc"
    cases.append(
        (
            S_BIN,
            label,
            partial(
                bin_case,
                bin_path=bin_path,
                sf=7,
                truth=b"Hello rtl_lora",
                cr=1,
                msg="Hello rtl_lora",
            ),
        )
    )

    # 2. every listed SNR per SF, NOISE_DRAWS draws each; all must decode
    for sf, snrs in NOISE_SNR.items():
        for snr in snrs:
            label = f"noise sf{sf} {snr:g} dB x{NOISE_DRAWS}"
            pay = rand_bytes(case_rng(seed, label), 192)
            cases.append(
                (
                    S_NOISE,
                    label,
                    partial(
                        noise_case,
                        bin_path=bin_path,
                        sf=sf,
                        snr=snr,
                        truth=pay,
                        seed0=case_rng(seed, label).randrange(1 << 20),
                        cr=1,
                        payload_hex=pay.hex(),
                    ),
                )
            )

    # 3. real captures: no modulator here can share a bug with the demodulator.
    # -w 0x20 checks netid1 only (a slip damages netid2); -w 0x2b checks both.
    for cap in ("capture1", "capture2", "capture3"):
        for netid, tag in ((None, ""), ("0x20", " -w"), ("0x2b", " -w2b")):
            cases.append(
                (
                    S_MULTI,
                    f"{cap} stream{tag}",
                    partial(stream_case, bin_path=bin_path, cap=cap, netid=netid),
                )
            )
        if per_frame:
            for i in range(CAP_FRAMES[cap]):
                key = f"{cap}/frame{i}"
                for netid, tag in ((None, ""), ("0x20", " -w"), ("0x2b", " -w2b")):
                    cases.append(
                        (
                            S_CAP,
                            key.replace("/", " ") + tag,
                            partial(capture_case, bin_path=bin_path, key=key, netid=netid),
                        )
                    )
    return cases


def noise_case(iq, bin_path, sf, snr, truth, seed0, **frame_args):
    """Every NOISE_DRAWS draw at `snr` must decode; seed0 + draw index makes a
    failure reproducible. `-a 1`: the detector has no absolute threshold, so at
    depth 4 noise-driven preamble runs slip past sync."""
    try:
        import numpy as np
    except ImportError as e:
        raise CaseSkip(f"numpy unavailable ({e}), so no AWGN")
    from common import add_awgn

    clean_iq = iq + ".clean"
    Frame(clean_iq, sf=sf, **frame_args).run()
    rx = Receiver(bin_path=bin_path, iq=iq, sf=sf, os_factor=2, avg=NOISE_AVG)
    clean = np.fromfile(clean_iq, dtype=np.complex64)

    good, first_bad = 0, None
    for d in range(NOISE_DRAWS):
        add_awgn(clean, snr, np.random.default_rng(seed0 + d)).astype(np.complex64).tofile(iq)
        try:
            check_out(rx.run(verdict=True), rx.argv(), truth)
        except CaseFail as e:
            first_bad = first_bad or e
        else:
            good += 1
    if first_bad is not None:
        raise CaseFail(
            f"{' '.join(rx.argv())}  (seed {seed0}..{seed0 + NOISE_DRAWS - 1})",
            f"{good}/{NOISE_DRAWS} draws decoded {len(truth)} B exact with CRC "
            f"valid at {snr:g} dB; all {NOISE_DRAWS} must. "
            f"First shortfall:\n{first_bad.out}",
        )
    return f"{NOISE_DRAWS}/{NOISE_DRAWS} draws {len(truth)} B exact"


def is_lfs_pointer(path):
    with open(path, "rb") as f:
        return f.read(45).startswith(b"version https://git-lfs.github.com")


def capture_receiver(bin_path, cap, src, netid=None):
    """Receiver configured for the committed captures."""
    return Receiver(
        bin_path=bin_path,
        iq=src,
        sf=9,
        os_factor=8,
        avg=1,
        center_freq=CAPTURE_CENTER_HZ,
        bw=CAPTURE_BW_HZ,
        input_rate=CAPTURE_FILE_RATE_HZ,
        conj=CAPTURE_CONJ[cap],
        netid=netid,
    )


def capture_case(iq, bin_path, key, netid=None):
    """One committed frame; CRC valid is the whole verdict."""
    cap, name = key.split("/")
    src = os.path.join(ROOT, "data", cap, f"{name}.cf32")
    if not os.path.exists(src):
        raise CaseSkip(f"{key} missing (is data/ checked out?)")
    if is_lfs_pointer(src):
        raise CaseSkip(f"{key} is an unfetched LFS pointer (git lfs pull)")

    rx = capture_receiver(bin_path, cap, src, netid)
    cp = rx.run(verdict=True)
    recs = parse_frames(cp.stdout)
    got = base64.b64decode(recs[0]["packet"]) if recs else None
    problems = []
    if not recs or recs[0]["crc_valid"] is not True:
        problems.append("CRC invalid")
    if got is None:
        problems.append("no RX")
    if problems:
        raise CaseFail(" ".join(rx.argv()), "; ".join(problems))
    return f"{len(got)} B, CRC valid"


def stream_case(iq, bin_path, cap, netid=None):
    """One recording, every frame re-armed back to back. capture3 uses the
    committed all_frames.cf32; the others are concatenated here."""
    if cap == "capture3":
        src = os.path.join(ROOT, "data", "capture3", "all_frames.cf32")
        if not os.path.exists(src):
            raise CaseSkip("capture3/all_frames.cf32 missing (is data/ checked out?)")
        if is_lfs_pointer(src):
            raise CaseSkip("capture3/all_frames.cf32 is an unfetched LFS pointer (git lfs pull)")
    else:
        names = [f"frame{i}" for i in range(CAP_FRAMES[cap])]
        srcs = [os.path.join(ROOT, "data", cap, n + ".cf32") for n in names]
        for p in srcs:
            if not os.path.exists(p):
                raise CaseSkip(f"{cap}/{os.path.basename(p)} missing (is data/ checked out?)")
            if is_lfs_pointer(p):
                raise CaseSkip(
                    f"{cap}/{os.path.basename(p)} is an unfetched LFS pointer (git lfs pull)"
                )
        native = iq + ".native"
        with open(native, "wb") as out:
            for p in srcs:
                with open(p, "rb") as f:
                    out.write(f.read())
        src = native

    rx = capture_receiver(bin_path, cap, src, netid)
    cp = rx.run(verdict=True)
    recs = parse_frames(cp.stdout)
    if not recs:
        raise CaseFail(" ".join(rx.argv()), (cp.stdout + cp.stderr).strip()[-1200:])

    got = [base64.b64decode(r["packet"]) for r in recs]
    valid = sum(r["crc_valid"] is True for r in recs)
    problems = []
    if len(got) != CAP_FRAMES[cap]:
        problems.append(f"decoded {len(got)} frames, want {CAP_FRAMES[cap]}")
    if valid != len(got):
        problems.append(f"{valid}/{len(got)} frames CRC valid")
    if problems:
        raise CaseFail(" ".join(rx.argv()), "; ".join(problems[:8]))
    return f"{len(got)} frames, all CRC valid"


def main():
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "1. clean — generated frames, SF 7..12, the payload ladder, "
            "--no-crc, a smoke frame\n"
            f"2. AWGN — every listed SNR, {NOISE_DRAWS} draws per point, all must decode\n"
            "3. captures — three Meshtastic recordings, three netid arms each\n"
            "Payloads are PRNG-seeded per case (--seed)."
        ),
    )
    p.add_argument("--seed", type=int, default=1, help="PRNG seed for generated payloads")
    p.add_argument(
        "--bin",
        default=os.path.join(ROOT, "bin", "file_lora"),
        help="binary under test (not built here)",
    )
    p.add_argument("--only", metavar="SUBSTR", help="run only cases whose label contains SUBSTR")
    p.add_argument("--list", action="store_true", help="print the matrix, unrun")
    p.add_argument("--no-capture", action="store_true", help="drop section 3 (needs numpy+scipy)")
    p.add_argument(
        "--per-frame",
        action="store_true",
        help="also run every capture frame as its own case "
        f"({sum(CAP_FRAMES.values())} frames x 3 netid arms — slow)",
    )
    args = p.parse_args()
    if not os.access(args.bin, os.X_OK):
        p.error(f"{args.bin} missing or not executable (build it first)")

    cases = build_cases(args.seed, os.path.abspath(args.bin), per_frame=args.per_frame)
    if args.no_capture:
        cases = [c for c in cases if c[0] not in (S_CAP, S_MULTI)]
    if args.only:
        cases = [c for c in cases if args.only in c[1]]

    if args.list:
        section = None
        for sec, label, _ in cases:
            if sec != section:
                section = sec
                print(f"\n== {sec} ==")
            print(f"  {label}")
        print(f"\n{len(cases)} cases")
        return

    n_fail = n_skip = 0
    t_start = time.perf_counter()
    print(f"file_lora regression  seed={args.seed}  {len(cases)} cases  bin={args.bin}")

    section = None
    with tempfile.TemporaryDirectory() as tmp:
        for i, (sec, label, fn) in enumerate(cases):
            if sec != section:
                section = sec
                print(f"\n== {sec} ==")

            iq = os.path.join(tmp, f"{i:02d}.cf32")
            t0 = time.perf_counter()
            try:
                note = fn(iq)
            except CaseSkip as e:
                dt = time.perf_counter() - t0
                n_skip += 1
                print(f"SKIP  {label:<26} {dt:6.1f}s  {e}")
                continue
            except CaseFail as e:
                dt = time.perf_counter() - t0
                n_fail += 1
                print(f"FAIL  {label:<26} {dt:6.1f}s")
                print(f"      cmd: {e.cmd}")
                for line in e.out.splitlines()[-12:]:
                    print(f"      | {line}")
                continue
            dt = time.perf_counter() - t0
            print(f"PASS  {label:<26} {dt:6.1f}s  {note}")

    total = time.perf_counter() - t_start
    n_pass = len(cases) - n_fail - n_skip
    print("\n----")
    tally = f"{n_pass}/{len(cases)} passed"
    if n_skip:
        tally += f", {n_skip} skipped"
    print(f"{tally} in {total:.1f}s")
    if n_skip:
        print("(skipped cases were not exercised; a skip is not a pass)")
    sys.exit(0 if n_fail == 0 else 1)


if __name__ == "__main__":
    main()
