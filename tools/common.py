#!/usr/bin/env python3
"""Canonical LoRa PHY kernels shared by the tools/ scripts.

One definition each of the phase-continuous upchirp (channel-centered, as in
a tuned SDR), its conjugate downchirp (the SFD / dechirp LO flavor), the
decimated demodulator LO, and the AWGN channel. The chirp-flavor pairing rule
lives here by construction: modulators, demodulators and detectors all import
this kernel, so they cannot drift apart.

References:
- arXiv:1906.04256 — the k^2/2 twist of the phase-continuous chirp
- arXiv:1912.11344 — dechirping with the conjugate base upchirp (the LO below)
"""

import os

import numpy as np


def max_jobs() -> int:
    """Worker processes to start by default: every core this process may use."""
    try:
        return len(os.sched_getaffinity(0))  # truth under cgroup/taskset limits
    except AttributeError:  # not Linux
        return os.cpu_count() or 1


def upchirp(symbol: int, sf: int, oversampling: int) -> np.ndarray:
    """LoRa up-chirp for `symbol`, centered on DC = the channel center freq."""
    n = (1 << sf) * oversampling  # samples per symbol
    k = np.arange(n) / oversampling  # chip index per sample
    f_inst = ((symbol + k) % (1 << sf)) / (1 << sf) - 0.5  # in units of B
    phase = 2 * np.pi * np.cumsum(f_inst) / oversampling
    return np.exp(1j * phase)


def downchirp(sf: int, oversampling: int) -> np.ndarray:
    """The SFD flavor: conjugate of the base upchirp (same pairing rule)."""
    return np.conj(upchirp(0, sf, oversampling))


def lo(sf: int, oversampling: int) -> np.ndarray:
    """Decimated demodulator local oscillator (n-point)."""
    n = 1 << sf
    return np.conj(upchirp(0, sf, oversampling)[::oversampling][:n])


def add_awgn(x: np.ndarray, snr_db: float, rng: np.random.Generator) -> np.ndarray:
    """Complex AWGN at the per-complex-sample SNR: 10*log10(mean|x|^2 / E|n|^2).

    Box-Muller over float32 uniforms: cos/sin of one angle make re and im
    independent, and the draw stays a pure function of (seed, size), so a
    --verdicts line does not move. The /2 is E|n|^2 = 2*sigma^2 for complex
    normals; u1 keeps (0, 1] so log() stays finite.
    """
    sigma = np.float32(np.sqrt(float(np.vdot(x, x).real) / x.size * 10 ** (-snr_db / 10) / 2))
    u1 = 1.0 - rng.random(x.size, dtype=np.float32)
    u2 = rng.random(x.size, dtype=np.float32)
    rad = np.sqrt(np.float32(-2.0) * np.log(u1))
    ang = np.float32(2.0 * np.pi) * u2
    buf = np.empty(2 * x.size, dtype=np.float32)
    buf[0::2] = rad * np.cos(ang)
    buf[1::2] = rad * np.sin(ang)
    noise = buf.view(np.complex64)
    noise *= sigma
    return noise + x
