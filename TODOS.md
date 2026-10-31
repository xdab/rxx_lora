# TODOS

> Only outstanding or planned work. Done work is deleted, never ticked; git history remembers it if needed.
> No particular order, priorities, statuses, assignees, changelogs. Entries are simply statements.
> Written for a stranger. Every entry must make sense cold without conversation context, including to an outside contributor or AI agent.

## Receiver: frame sync (SFD-first)

The DETECT fire runs the SFD-first slot pipeline (`sfd_locate`, `src/sfd.c`):
coherent lock over the certified preamble run → change-point score with
positive downchirp evidence → pairing `τ = wrap(û − d̂)/2`, `f = wrap(û − τ)`
(N/2 ambiguity resolved by `|f| ≤ N/4`) → two-pass re-time → known-bin tests
scored in noise-power units (Λ) → Λ gate (noise-calibrated p₀.₀₀₁ at 32.5,
worded mode) arbitrates whether the fire was a frame at all. The walk starts
on the reference's window lattice (fire + `os·(N − k_hat)` mod sps), with the
pipeline deciding where on that lattice netid1 sits; `netid_verdict`'s
early-walk/foreign/nibble logic is the verdict on the read. The Python
prototype is `tools/sfd_hunt.py`.

Measured end-to-end (paired, common seeds, 1500 trials/point, `tools/ber.py`,
SF9 os=2, 8 B payload: port vs prior chain): PER 1.4% vs 3.0% at −14 dB
per-sample, 6.7% vs 11.0% at −15, 68.4% vs 72.9% at −17; the port decodes
clean from −12 dB where the prior chain still errs at −11. Sync-loss bucket
at −17 dB: 35.3% (300 trials, `tools/sync_decomp.py`).
`tools/regression.py` 41/41; `tools/regression_perf.py` worst case 9× RT.

Remaining, in order:

1. **Coarse-pass robustness.** The true hypothesis is missing from the fine
   pass's candidate set in 6 / 17 / 56% of prototype trials at eff
   −11 / −14 / −17 in-band — the coarse `d̂` (one argmax over the summed
   two-slot up-ref spectrum) feeding the τ pairing is the limiter. Wider D
   summation, or a coarse τ that does not rest on a single argmax.
2. **Λ gate calibration on the C build.** The gate runs the prototype's
   threshold (32.5, noise-only p₀.₀₀₁); the C score distribution can differ
   (float vs double, the 4N slot FFT, the representative-wide noise floor).
   Recalibrate noise-only Λ/margin thresholds for this build, then measure
   Pd/Pfa vs SNR and the foreign-frame flagging statistic before trusting
   the gate at depth (accept-any mode skips the gate by design — no
   expected words, other scale).
3. **DETECT interface.** Feed the measured DETECT fire-time distribution
   (incl. its early-fire / reset-delayed cases) into the lock-window
   assumption (`SFD_Q_MIN`/`SFD_Q_MAX` in `src/sfd.c` are the current guess).

### Chase retry: CRC-codeword flips

The chase retry is landed (`chase_retry`, `src/receiver.c`; margin gating
`CHASE_MARG`=0.2, LLR-loss arbitration `CHASE_TAU`=1.0 baked as defaults, K=4).
This entry is what it still cannot do. About 10 frames per 24k (SF9 sweep,
`data/sweep.py`) fail the payload CRC with a byte-perfect payload: the four
CRC codewords themselves decoded wrong. The chase never flips them (they are
the check), so those frames are unrescuable, and any retry attempted on one
can only manufacture a corrupt payload that matches the bad CRC. Intended
shape: add the CRC codewords to the flip candidate set, but strictly under
the LLR-loss arbitration — a CRC-nibble flip on a byte-perfect payload has
zero payload loss, the most plausible solution in the whole candidate set,
while a collision must also look metrically plausible. Risk: corrupt payloads
gain a new path to consistency, so the false-accept rate must be measured
before/after on the paired sweep harness (false accepts are the crc-ok
manifest records with errs>0 — the CRC-reported PER alone will happily call
them rescues).

Measured context to not re-derive: flip deltas are structured (XORs of
Hamming codeword nibble deltas at whitened positions), so a wrong combo
passes CRC at ~1e-3..1e-2 per attempt, not the naive 2^-16 — the LLR-loss
budget is the working defense, and any arbiter change is judged on
truth-aware PER, never the CRC-reported one. False accepts are also not
linear in attempts: they are the count of frames with any reachable
collision inside the attempted delta set, so small attempt budgets saturate
fast (~24 per 24k for 1-4 attempts) and then jump (~50 at 8 pair attempts).

### Chase defaults beyond SF9 cr4/5 8B

The baked defaults (K=4, `CHASE_MARG`=0.2, `CHASE_TAU`=1.0) were calibrated
on one geometry only: SF9, CR 4/5, 8 B payloads, -21..-14 dB per-sample
(`data/sweep.py`, paired 3000-trial sweeps). The τ budget is in absolute LLR
units and the LLR scale moves with SNR and the demod's metric calibration,
so τ's reach may differ at other SF/CR/payload lengths. Re-run the
calibration grid (`CHASE_MARG`/`CHASE_TAU` via `bin/file_lora.exp`, 300
trials/point, then confirm at 3000) per geometry before trusting the
defaults elsewhere; if the absolute τ does not generalize, normalize it (e.g.
by the frame's median codeword LLR mass) instead of stacking per-geometry
constants.

### Header list decoding

Role unchanged: header-checksum failures are a small bucket (3.8% at
−17 dB) and the arbitration partner of everything upstream — the chase
already uses it, and every frame the sync gate accepts lands on it next. A
soft header decode — keep the top-K codeword candidates per block, try
combinations against the 5-bit checksum (`decode_header`, `src/header.c`;
1/32 false-accept per wrong candidate, payload CRC arbitrates in the end),
same margin bookkeeping as the landed chase — rescues the genuine 3.8% and
widens the arbitration window the net-id recovery relies on. Size it by the
sync gate's own header-failure buckets.

### Soft preamble detection (DETECT)

DETECT's fire position is the `near` interface the slot pipeline locks
onto; feeding the measured fire-time distribution into the lock-window
assumption is item 3 of the frame-sync entry above.

The original entry stands for the hard/soft boundary: the header and
payload are fully soft (`demod_soft_llrs` → `deinterleave_soft` →
`hamming_decode_soft`, wired in `src/receiver.c`), while DETECT and SYNC
decide hard on uncoded preamble structure. The intended shape is
accumulating soft evidence, e.g. spectral intersection as an element-wise
min over the preamble's dechirped magnitude spectra, firing on a
likelihood threshold — a rearrangement of magnitudes DETECT already
computes, not a second transform, not a larger K (arithmetic caps K ≤ 4
for any preamble length), not a learned detector. Whether it buys range
or only false-alarm margin is decided by `tools/c_fa.py` and the per-stage
attribution; the two trade off.

### Joint net-id read: one foreign false-accept corner

The joint pair read (src/sync.c `decode_netid_pair`) converts 1/300
foreign frames (0x2b symbols against a `-w 0x12` filter, −17 dB) that the
plain argmax read rejects: a weak blade pair at spacing inside the
verdict's mod-N/4 ±2 window passes, and the early-walk branch accepts it.
The 6 dB per-member power gate did not stop this one (its blade was within
6 dB). Fix shape: tighten the joint spacing window or the early-walk
interaction, re-certify foreign rejection at 0.0 (the harness:
`data/sweep.py --netid X --gen-netid Y` cross runs, both binaries).

## Working rate and sampling architecture

### os=2 as the working rate

The fractional-delay taps are in (`frame_sync_frac_gather()`, `src/frame_sync.c`): STO/SFO apply at
sub-chip precision in the payload prologue and the net-id pair read, `-o2` matches `-o8` on the
decode floor (AWGN+CFO cliff via `tools/ber.py`; SFO-drift sweeps; `data/capture*` re-decodes), and
os=1 is out of spec — critically sampled, the gather interpolates a full-band signal and pays for
it. Outstanding here: flip the Python tools' `--os 8` default to 2 (the defaults-mismatch note in
AGENTS.md goes with it), and sweep the resampler kernel at `-o2` (`RESAMP_M` 6–8, `make
experimental`): the freed rate budget buys a sharper cutoff, and the sweep decides whether that
buys real frames or only margin — at `-o2` the rate-mismatched leg measures no band-edge loss.
Detect at os=2 (window advance of N*2) needs no attention: `k_hat` is whole-bin regardless of os,
and detect-failure counts in the BER sweep show no os trend. Criticality: a config and tooling flip
plus a measurement.

### Working-band noise rejection

Hypothesis: the chain's effective noise bandwidth is the whole `bw*os` working band, not `bw` — the
decimation gather (`frame_sync_downsample_prologue()`, `src/frame_sync.c`) takes one sample per chip
with no anti-alias filtering, so out-of-signal-band noise rides into the demod. Evidence: on
noise-PSD-matched synthetic frames the decode floor scales ~10·log10(os) with os; on real captures,
decoding a file at its native rate — resampler skipped, `data/baleya*_o4` at `-o4`, `baleya3_o7` at
`-o7` — decodes fewer frames than the same file resampled to any other os, because skipping the
resampler also skips its anti-aliasing (~3 dB of per-sample noise on a 1 MHz file). Intended shape:
the fractional gather's kernel doubles as the decimation anti-alias filter — a Kaiser-sinc with a
cutoff at the chip-rate Nyquist, kernel length growing with os — or a cheap fixed band-limiter
before detect where the working rate exceeds 2x the signal band. Not the intended shape: not
touching the stream resampler (`src/resample.c`) — this entry is about the rate-matched path where
it never runs. Criticality: the native-rate dip is real frames on real captures; the size of the win
at the intended `-o2` working rate is the open question (there the working band is only 2x the
signal band, so the existing kernel may already be close).

### Native-rate fractional gather

Hypothesis: the rigid `bw*os` working grid can be dissolved entirely — the ring kept at the device's
native rate, every window gathered at fractional positions from a single NCO accumulating rate-ratio +
STO + SFO — deleting the stream resampler altogether; its anti-aliasing role (it is the only filter
before detect, `src/resample.c`) folds into the gather kernel or a cheap fixed decimator where the
native rate exceeds 2x the signal band. The fractional-delay gather is the first half of this;
promoting it to also do the rate conversion is the real refactor — `frame_sync`'s internal buffers
(`preamble_upchirps`, `net_id_samp`, ...) are all working-grid-sized. Criticality: a wish, and a
lowered one — the attribution outcome says the resampler's transition band only bites at os=1 (out
of spec); at os>=2 the filter term measured zero, so the resampler's job that matters is
anti-aliasing (see the working-band noise entry), not transition-band fidelity.

### Naive vs advanced receive chain, switchable

The receive chain should show a clean split between a naive, textbook implementation and the advanced one. The two are switched either by environment variable at runtime (for experiments) or by preprocessor macro at compile time. The best receiver is enabled by default, and the split is visible either way: the idiomatic, textbook LoRa implementation lives entirely outside the conditional blocks, while the advancements and optimizations sit inside them (`#if` or a plain `if()`) as a kind of feature flag.

Criticality: a readability and experimentation item — it makes which behavior is which legible, with no effect on the default result.

## Buffering and copies

### Mirror-mapped wrap-around ring

Problem: the ring is linear with a dead prefix — `ring_drop` advances `start`, and once the prefix crosses a threshold `ring_compact` memmoves the whole live region to the front. The compaction is pure overhead paid forever; at `-o8` it measured 31.7M Ir (1.2%) on a rate-matched decode, the largest remaining copy after ingress and the DETECT archive went zero/decimated-copy.

Intended shape: a wrap-around ring with power-of-two capacity whose pages are mapped twice consecutively (`mmap`, POSIX-only): any span up to capacity stays contiguous in address space across the wrap, so `ring_peek` still returns a single pointer, ingress and reads are zero-copy, and nothing ever moves. Combined with pre-sizing, sample pointers become stable for their whole buffered life.

Not the intended shape: not a modulo ring with two-segment reads — the receiver is built on contiguous window pointers; and not a larger compaction threshold as a "fix" — that only trades memory for a linear reduction.

Open question: whether the mmap dependency is worth it at our window sizes, or whether the linear ring minus the ingress copies is already below the noise floor — decide by measurement, not aesthetics. Measured context (callgrind A/B of the pre-sized ring): steady-state residency is bounded by one frame span, not the window geometry — the hunt advances per window, but a decoding frame rewinds `pos` to the fire position on rejection (`receiver.c`), so the ring must hold fire-to-frame-end; a length-255 payload at `-o8` is ~3.4M samples. Ring growth reallocs on a capture are therefore frame-length-driven (a couple, at stream start), and glibc extends large reallocs via mremap — the copies the linear ring was blamed for largely do not exist.
