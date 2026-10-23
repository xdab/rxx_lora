#!/usr/bin/env python3
"""LoRa frame generator demo: symbol-level PHY frame -> cf32 file (+ plot)."""

import argparse
import os
import sys
from itertools import pairwise

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# gr-lora_sdr tables.h whitening_seq
WHITENING_SEQ = bytes.fromhex(
    "fffefcf8f0e1c2850b172f5ebc78f1e3c68d1a3468d0a040"
    "80010204081123478e1c3871e2c48912254b972e5cb870e0"
    "c08103060c193264c992244993264d9b376edcb972e4c890"
    "204182050a152b56ad5bb66ddab56bd6ac59b265cb962c58"
    "b061c3870f1f3e7dfbf6eddbb76fdebd7af5ebd7ae5dba74"
    "e8d1a24488102143860d1b366cd8b163c78f1e3c79f3e7ce"
    "9c3973e6cc983162c58b162d5ab469d2a4489122458a1429"
    "52a54a952a54a953a74e9d3b77eeddbb76ecd9b367cf9e3d"
    "7bf7efdfbf7efdfaf4e9d3a64c993366cd9a356ad4a851a3"
    "468c183060c183070e1d3a75ead5aa55ab57af5fbe7cf9f2"
    "e5ca942850a142840913274f9f3f7f"
)


def crc16(data: bytes) -> int:
    """CRC-16/XMODEM (poly 0x1021, init 0), as crc_verif_impl.cc:49-71."""
    crc = 0
    for byte in data:
        for _ in range(8):
            if ((crc & 0x8000) >> 8) ^ (byte & 0x80):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
            byte <<= 1
    return crc


def payload_crc(msg: bytes) -> int:
    """LoRa payload CRC: crc16 over the first N-2 bytes, XOR the last two
    (add_crc_impl.cc:121-124) — undefined for messages shorter than 2."""
    return crc16(msg[: len(msg) - 2]) ^ msg[-1] ^ (msg[-2] << 8)


def header_nibbles(pl: int, cr: int, has_crc: bool) -> list[int]:
    """The 5 PHY-header nibbles (header_impl.cc:114-132)."""
    hdr = [pl >> 4, pl & 0x0F, (cr << 1) | int(has_crc)]
    c4 = (hdr[0] & 8) >> 3 ^ (hdr[0] & 4) >> 2 ^ (hdr[0] & 2) >> 1 ^ (hdr[0] & 1)
    c3 = (
        (hdr[0] & 8) >> 3 ^ (hdr[1] & 8) >> 3 ^ (hdr[1] & 4) >> 2 ^ (hdr[1] & 2) >> 1 ^ (hdr[2] & 1)
    )
    c2 = (
        (hdr[0] & 4) >> 2 ^ (hdr[1] & 8) >> 3 ^ (hdr[1] & 1) ^ (hdr[2] & 8) >> 3 ^ (hdr[2] & 2) >> 1
    )
    c1 = (
        (hdr[0] & 2) >> 1
        ^ (hdr[1] & 4) >> 2
        ^ (hdr[1] & 1)
        ^ (hdr[2] & 4) >> 2
        ^ (hdr[2] & 2) >> 1
        ^ (hdr[2] & 1)
    )
    c0 = (
        (hdr[0] & 1)
        ^ (hdr[1] & 2) >> 1
        ^ (hdr[2] & 8) >> 3
        ^ (hdr[2] & 4) >> 2
        ^ (hdr[2] & 2) >> 1
        ^ (hdr[2] & 1)
    )
    return hdr + [c4, c3 << 3 | c2 << 2 | c1 << 1 | c0]


def hamming_enc_nibble(nib: int, cr: int) -> int:
    """One nibble -> one codeword of cr+4 bits (hamming_enc_impl.cc:96-115)."""
    b = [(nib >> 3) & 1, (nib >> 2) & 1, (nib >> 1) & 1, nib & 1]
    if cr == 1:  # 4/5: single total-parity bit
        p4 = b[0] ^ b[1] ^ b[2] ^ b[3]
        return b[3] << 4 | b[2] << 3 | b[1] << 2 | b[0] << 1 | p4
    p0 = b[3] ^ b[2] ^ b[1]
    p1 = b[2] ^ b[1] ^ b[0]
    p2 = b[3] ^ b[2] ^ b[0]
    p3 = b[3] ^ b[1] ^ b[0]
    return (b[3] << 7 | b[2] << 6 | b[1] << 5 | b[0] << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3) >> (
        4 - cr
    )


def gray_demap_sym(v: int, sf: int) -> int:
    """TX gray demap: XOR-fold then +1 (gray_demap_impl.cc:62-71)."""
    g = v
    for j in range(1, sf):
        g ^= v >> j
    return (g + 1) % (1 << sf)


def interleave(codewords: list[int], sf: int, cr: int) -> list[int]:
    """Codeword stream -> LoRa symbols (interleaver_impl.cc, LDRO off).

    Input is the hamming_enc output: the first sf-2 nibbles carry CR 4/8
    codewords (8 bits), the rest CR 4/(4+cr) (cr+4 bits). The first block
    (the 5 header nibbles' codewords) runs sf_app=sf-2 plus a row-parity
    bit; bool2int runs over all sf bits, leaving a trailing zero."""
    out: list[int] = []
    cw_cnt = pos = 0
    while pos < len(codewords):
        first = cw_cnt < sf - 2
        cw_len = 4 + (4 if first else cr)
        sf_app = sf - 2 if first else sf
        blk = [codewords[pos + i] if pos + i < len(codewords) else 0 for i in range(sf_app)]
        pos += sf_app
        cw_cnt += sf_app
        rows = [[(v >> (cw_len - 1 - k)) & 1 for k in range(cw_len)] for v in blk]
        for i in range(cw_len):
            bits = [rows[(i - j - 1) % sf_app][i] for j in range(sf_app)]
            if cw_cnt == sf - 2:  # first block: parity bit, zero in the end
                bits.append(sum(bits) % 2)
            v = 0
            for b in bits + [0] * (sf - len(bits)):  # bool2int over the m_sf-bit row
                v = (v << 1) | b
            out.append(v)
    return out


def hamming_encode_stream(nibbles: list[int], sf: int, cr: int) -> list[int]:
    """hamming_enc block: first sf-2 nibbles at CR 4/8, the rest at CR
    4/(4+cr) (hamming_enc_impl.cc:97, m_cnt < m_sf - 2)."""
    return [hamming_enc_nibble(n, 4 if i < sf - 2 else cr) for i, n in enumerate(nibbles)]


def coded_nibbles(msg: bytes, cr: int, has_crc: bool) -> list[int]:
    """Header + whitened payload + raw CRC nibbles, the hamming_enc input
    (whitening_impl.cc:178-181, header_impl.cc:141-154, add_crc_impl.cc:119-133)."""
    pl = len(msg)
    nib = header_nibbles(pl, cr, has_crc)
    for i, byte in enumerate(msg):
        w = byte ^ WHITENING_SEQ[i]
        nib += [w & 0x0F, w >> 4]
    if has_crc:
        crc = payload_crc(msg)
        nib += [crc & 0xF, (crc >> 4) & 0xF, (crc >> 8) & 0xF, (crc >> 12) & 0xF]
    return nib


_CHIRP_CACHE: dict[tuple[int, int, int], np.ndarray] = {}
_CHIRP_CACHE_BYTES = 0
# Bounded: a chirp is sps*16 bytes, so SF12 x os=8 is 0.5 MB each and an
# unbounded 2^sf cache would be gigabytes. Past the budget we just rebuild.
_CHIRP_CACHE_BUDGET = 64 << 20


def ref_upchirp(symbol: int, sf: int, oversampling: int) -> np.ndarray:
    """Reference-flavor upchirp (gr-lora_sdr utilities.h build_upchirp):
    closed-form quadratic phase, the exact samples of the C chain's
    util_build_upchirp. Memoized; the returned array is SHARED — copy
    before mutating."""
    global _CHIRP_CACHE_BYTES
    key = (sf, oversampling, symbol)
    hit = _CHIRP_CACHE.get(key)
    if hit is not None:
        return hit
    n = 1 << sf
    n_fold = n * oversampling - symbol * oversampling
    i = np.arange(n * oversampling)
    phase = np.where(
        i < n_fold,
        2 * np.pi * (i * i / (2.0 * n) / oversampling**2 + (symbol / n - 0.5) * i / oversampling),
        2 * np.pi * (i * i / (2.0 * n) / oversampling**2 + (symbol / n - 1.5) * i / oversampling),
    )
    y = np.exp(1j * phase).astype(np.complex64)
    if _CHIRP_CACHE_BYTES + y.nbytes <= _CHIRP_CACHE_BUDGET:
        _CHIRP_CACHE[key] = y
        _CHIRP_CACHE_BYTES += y.nbytes
    return y


def build_frame(pre: int, sync: list[int], payload: list[int], sf: int, os: int) -> np.ndarray:
    """Concatenate the frame elements; return the complex baseband samples.

    Reference (gr-lora_sdr) conventions: sync words raw
    (modulate_impl.cc:172-174), payload symbols as s+1, which the
    demodulator's mod(sym-1, N) undoes (fft_demod_impl.cc:301).
    """
    sps = (1 << sf) * os
    n = 1 << sf
    up0 = ref_upchirp(0, sf, os)
    down = np.conj(up0)  # SFD flavor: conjugate of the same up-chirp
    parts = [up0] * pre
    parts += [ref_upchirp(s, sf, os) for s in sync]
    parts += [down, down, down[: sps // 4]]  # 2.25 downchirps
    parts += [ref_upchirp((s + 1) % n, sf, os) for s in payload]
    return np.concatenate(parts)


def build_frame_coded(
    pre: int, sync: list[int], msg: bytes, cr: int, has_crc: bool, sf: int, os: int
) -> tuple[np.ndarray, int]:
    """Full TX chain frame (gr-lora_sdr TX flowgraph): hamming_enc ->
    interleaver -> gray_demap -> upchirp modulation over coded_nibbles().
    Returns (samples, number of payload-area symbols)."""
    sps = (1 << sf) * os
    up0 = ref_upchirp(0, sf, os)
    down = np.conj(up0)  # SFD flavor: conjugate of the same up-chirp
    syms = interleave(hamming_encode_stream(coded_nibbles(msg, cr, has_crc), sf, cr), sf, cr)
    parts = [up0] * pre
    parts += [ref_upchirp(s, sf, os) for s in sync]
    parts += [down, down, down[: sps // 4]]  # 2.25 downchirps
    parts += [ref_upchirp(gray_demap_sym(v, sf), sf, os) for v in syms]
    return np.concatenate(parts), len(syms)


def plot_frame(
    x: np.ndarray, pre: int, nsync: int, npay: int, sf: int, bw: float, os: int, path: str
) -> None:
    """Three-panel view with the frame regions annotated (imports matplotlib
    lazily: never used by the sweep harnesses that import this module)."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import mlab

    fs = bw * os
    sps = (1 << sf) * os
    t = np.arange(len(x)) / fs

    # region [start, end) in samples: preamble | sync | SFD (2.25 sym) | payload
    bounds = [0, pre * sps, (pre + nsync) * sps]
    bounds += [bounds[-1] + 2 * sps + sps // 4, bounds[-1] + npay * sps]
    regions = list(pairwise(bounds))
    names = ["preamble", "sync", "SFD", "payload"]
    colors = ["#4c72b0", "#dd8452", "#55a868", "#c44e52"]

    # element boundaries for the symbol grid lines (SFD has 3 elements)
    edges = [0]
    for s, e in zip(bounds, [pre * sps, (pre + 2) * sps, bounds[2] + sps // 4]):
        edges += [s + sps, e] if e - s > sps else [e]
    edges += [bounds[3] + i * sps for i in range(1, npay + 1)]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(20, 6.5), constrained_layout=True)

    for ax in (ax1, ax2):
        for (s, e), nm, c in zip(regions, names, colors):
            ax.axvspan(s / fs * 1e3, e / fs * 1e3, color=c, alpha=0.10)
        for e in edges:
            ax.axvline(e / fs * 1e3, color="k", lw=0.5, alpha=0.4)

    # per-element inst. freq; the chirps are natively centered (-B/2 .. +B/2)
    lens = [sps] * pre + [sps] * nsync + [sps, sps, sps // 4] + [sps] * npay
    starts = np.concatenate([[0], np.cumsum(lens)[:-1]])
    f_inst = np.concatenate(
        [
            np.diff(np.unwrap(np.angle(x[s : s + l]))) / (2 * np.pi) * fs
            for s, l in zip(starts, lens)
        ]
    )
    t_inst = np.concatenate([t[s + 1 : s + l] for s, l in zip(starts, lens)])
    ax1.plot(t_inst * 1e3, f_inst / 1e3)
    ax1.set_xlabel("time [ms]")
    ax1.set_ylabel("inst. freq [kHz]")
    ax1.set_title(
        f"LoRa frame, SF={sf}, BW={bw / 1e3:g} kHz, DC = channel center: "
        "preamble ridges, sync ramps, SFD falling, payload sawtooth"
    )
    ax1.set_ylim(-bw / 2 / 1e3, bw / 2 / 1e3)
    ax1.grid(alpha=0.3)

    # waterfall: mlab already returns the two-sided spectrum centered (rows
    # and freqs aligned, DC in the middle) — plot it as-is
    pxx, freqs, tbins = mlab.specgram(x, NFFT=128, noverlap=112, Fs=fs)
    pdb = 10 * np.log10(pxx + 1e-13)
    pm = ax2.pcolormesh(
        tbins * 1e3, freqs / 1e3, pdb, vmin=pdb.max() - 40, cmap="viridis", shading="auto"
    )
    ax2.set_xlabel("time [ms]")
    ax2.set_ylabel("freq [kHz], DC = channel center")
    ax2.set_title("channel waterfall (SDR-style)")
    ax2.set_ylim(-0.6 * bw / 1e3, 0.6 * bw / 1e3)
    fig.colorbar(pm, ax=ax2, label="dB")

    for ax in (ax1, ax2):
        for (s, _), nm in zip(regions, names):
            ax.text(
                (s + sps * 0.1) / fs * 1e3,
                ax.get_ylim()[1] * 0.97,
                nm,
                fontsize=8,
                alpha=0.7,
                va="top",
            )

    fig.savefig(path, dpi=150)
    print(f"wrote {path}")


def generate(
    sf: int,
    os_: int,
    pre: int,
    sync: list[int],
    payload: list[int] | None = None,
    msg: bytes | None = None,
    cr: int | None = None,
    has_crc: bool = True,
    pad: int = 0,
    sto: int = 0,
    cfo: float = 0.0,
) -> tuple[np.ndarray, int]:
    """Build one frame in-process: preamble + sync + SFD + payload.
    The single implementation behind both the CLI and the sweep harnesses.
    Returns (samples, payload symbol count); the caller writes the file."""
    n = 1 << sf
    for s in sync + (payload or []):
        if not 0 <= s < n:
            raise ValueError(f"symbol {s} out of range [0, {n}) for SF={sf}")
    if len(sync) != 2:
        raise ValueError(f"sync needs exactly 2 symbols, got {sync}")
    if cr is not None:
        if sf < 7:
            raise ValueError("coded frames need SF >= 7 (the header block holds sf-2 nibbles)")
        if msg is None:
            raise ValueError("coded frames need a message")
        if has_crc and len(msg) < 2:
            raise ValueError("coded frames with CRC need a message of at least 2 bytes")
        if not 1 <= len(msg) <= 255:
            raise ValueError(f"message length {len(msg)} out of range 1..255 (8-bit header)")
        x, n_pay_syms = build_frame_coded(pre, sync, msg, cr, has_crc, sf, os_)
    else:
        n_pay_syms = len(payload)
        x = build_frame(pre, sync, payload, sf, os_)
    nbody = len(x)
    lead = pad + sto
    if lead or pad:
        framed = np.zeros(lead + nbody + pad, dtype=np.complex64)
        framed[lead : lead + nbody] = x
        x = framed
    if cfo:
        # x * e^{j2pi cfo t} over the body only; the ramp keeps the body's
        # absolute sample index, so the CFO lands where a whole-frame
        # rotation would put it
        th = np.arange(lead, lead + nbody, dtype=np.float32)
        th *= np.float32(2.0 * np.pi * cfo / (n * os_))
        rot = np.empty(nbody, dtype=np.complex64)
        np.cos(th, out=rot.real)
        np.sin(th, out=rot.imag)
        x[lead : lead + nbody] *= rot
    return x, n_pay_syms


def main() -> None:
    p = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__.split("\n\n")[0],
        epilog=(
            "Builds a complete LoRa PHY frame per the reverse-engineered "
            "packet structure — purely at the symbol level, no bits, bytes, "
            "headers or coding:\n"
            "    N_pr unmodulated upchirps (preamble, s = 0, length 6..65535)\n"
            "    2 network-id symbols   (sync word; default 8,16 = private "
            "0x12)\n"
            "    2.25 downchirps        (SFD: two full + one quarter symbol)\n"
            "    payload symbols        (back-to-back upchirps; the frame ends "
            "here)\n"
            "No postamble: termination is length-based. Since the C port the "
            "frame is written in the gr-lora_sdr flavor (closed-form "
            "quadratic-phase chirps, the exact samples of util_build_upchirp) "
            "with the reference value conventions — sync words raw, payload as "
            "s+1 — so the C receiver demodulates it exactly. The value-0 "
            "preamble/SFD elements dechirp "
            "identically under both flavors.\n"
            "--plot renders the frame SDR-style (DC = the channel center "
            "frequency): preamble ridges, sync ramps, SFD descending, payload "
            "sawtooth.\n"
            "The receiver that has to agree with all this is bin/file_lora; "
            "regression.py drives it with frames from here."
        ),
    )
    p.add_argument("--pre", type=int, default=8, help="preamble length in upchirps (6..65535)")
    p.add_argument(
        "--sync",
        default="8,16",
        help="two network-id symbols, comma-separated (default 8,16 = the "
        "0x12 private sync word, per gr-lora_sdr's nibble<<3 mapping)",
    )
    p.add_argument(
        "--payload",
        default="7,42,100,0,127",
        help="comma-separated payload symbols, each in [0, 2^SF) (uncoded mode)",
    )
    p.add_argument(
        "--msg",
        default="Hello rtl_lora",
        help="payload string for coded frames (--cr given); bytes are "
        "whitened, CRC'd, hamming-encoded, interleaved and gray-mapped "
        "per the gr-lora_sdr TX chain",
    )
    p.add_argument(
        "--hex",
        default=None,
        metavar="HEX",
        help="payload as hex octets for coded frames, overriding --msg — "
        "arbitrary binary payloads (any byte value) for BER trials, e.g. "
        "--hex 00FF7FDEADBEEF",
    )
    p.add_argument(
        "--cr",
        type=int,
        default=None,
        choices=(1, 2, 3, 4),
        help="coding rate 4/(4+cr): given, emit an explicit-header coded "
        "frame instead of the uncoded --payload frame (SF >= 7)",
    )
    p.add_argument(
        "--no-crc",
        action="store_true",
        help="omit the payload CRC (coded frames; CRC is on by default)",
    )
    p.add_argument(
        "--pad",
        type=int,
        default=0,
        help="exact zeros prepended before AND appended after the frame",
    )
    p.add_argument(
        "--cfo",
        type=float,
        default=0.0,
        help="carrier frequency offset in FFT bins, fractional allowed "
        "(can be negative; exact .5 offsets are bin-ambiguous)",
    )
    p.add_argument(
        "--sto",
        type=int,
        default=0,
        help="sampling time offset in samples (frame starts this much later; "
        "deliberately indistinguishable from --pad, like in a real receiver)",
    )
    p.add_argument("--sf", type=int, default=7, help="spreading factor (7..12)")
    p.add_argument("--bw", type=float, default=125e3, help="bandwidth in Hz")
    p.add_argument("--os", type=int, default=8, help="oversampling (samples/chip)")
    p.add_argument("--iq", default="frame.cf32", help="output cf32 file (float32 I/Q)")
    p.add_argument(
        "--plot",
        nargs="?",
        const="frame.png",
        default=None,
        metavar="PNG",
        help="also render time/inst-freq/waterfall PNG",
    )
    args = p.parse_args()

    n = 1 << args.sf
    if not 6 <= args.pre <= 65535:
        p.error(f"--pre {args.pre} out of range 6..65535")
    if args.pad < 0:
        p.error("--pad must be >= 0")
    if args.sto < 0:
        p.error("--sto must be >= 0")
    sync = [int(s) for s in args.sync.split(",")]
    payload = [int(s) for s in args.payload.split(",")]

    msg = None
    if args.cr is not None:
        if args.hex is not None:
            try:
                msg = bytes.fromhex(args.hex)
            except ValueError:
                p.error(f"--hex {args.hex!r} is not a hex octet string")
        else:
            msg = args.msg.encode()

    try:
        x, n_pay_syms = generate(
            sf=args.sf,
            os_=args.os,
            pre=args.pre,
            sync=sync,
            payload=payload,
            msg=msg,
            cr=args.cr,
            has_crc=not args.no_crc,
            pad=args.pad,
            sto=args.sto,
            cfo=args.cfo,
        )
    except ValueError as e:
        p.error(str(e))
    x.astype(np.complex64).tofile(args.iq)

    sps = n * args.os
    total = args.pre + len(sync) + n_pay_syms
    dur = len(x) / (args.bw * args.os) * 1e3
    print(
        f"SF={args.sf}  os={args.os}  frame={args.pre}+{len(sync)}+2.25+"
        f"{n_pay_syms} symbol periods ({len(x)} = {total}*{sps} + {sps // 4})"
    )
    print(f"wrote {len(x)} samples ({dur:.3f} ms @ {args.bw / 1e3:g} kHz BW) to {args.iq}")

    if args.plot:
        plot_frame(x, args.pre, len(sync), n_pay_syms, args.sf, args.bw, args.os, args.plot)


if __name__ == "__main__":
    main()
