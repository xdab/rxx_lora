# TODOS

> Only outstanding or planned work. Done work is deleted, never ticked; git history remembers it if needed.
> No particular order, priorities, statuses, assignees, changelogs. Entries are simply statements.
> Written for a stranger. Every entry must make sense cold without conversation context, including to an outside contributor or AI agent.

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

## Working rate and sampling architecture

### os=2 as the working rate

The fractional-delay taps are in (`frame_sync_frac_gather()`, `src/frame_sync.c`): STO/SFO apply at
sub-chip precision in the payload prologue and the net-id pair read, `-o2` matches `-o8` on the
decode floor (AWGN+CFO cliff via `tools/ber.py`; SFO-drift sweeps; `data/capture*` re-decodes), and
os=1 is out of spec — critically sampled, the gather interpolates a full-band signal and pays for
it. The Python tools now default to `--os 2` (matching the binaries). Detect at os=2 (window
advance of N*2) needs no attention: `k_hat` is whole-bin regardless of os, and detect-failure
counts in the BER sweep show no os trend. The `RESAMP_M` kernel sweep at `-o2` measured a no-op —
at the rate-matched working rate the resampler never runs; the kernel's effect lives on the
rate-mismatched leg (see working-band noise rejection).

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
