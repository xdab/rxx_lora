# plan.md — resampler campaign candidate list (working notes)

The post-campaign profile (809.5M Ir total, `data/prof/camp2-callgrind.out`)
is a one-horse race: `resample.c:resampler_push` at 442.9M self Ir = 54.7%
of the whole profile. Everything else is <=12.2% and the tail past #2 is
flat (<=4.3%).

## Ground truth (callgrind, self Ir)

- 183.2M MAC: `acc += h * rs->w[oldest]` (83.2M) + the tap loop (45.0M)
  + the ring wrap (27.7M) + the drained/group-delay check (26.4M)
- 69.3M coefficient interpolation: `h = h0[k] + mu * (h1[k] - h0[k])`
  per tap, per output — as expensive as the MAC itself
- structure: 8-tap branches (h_sub), one output every `step` input
  samples, mu fractional advance; capture1 ratio is exactly 125/256
  (1.024 MHz -> bw*os = 500 kHz)

Prior results that bind (measured, from the previous campaign):

- auto-vectorization: +24% wall — REJECTED
- hand AVX2+FMA SoA: -3% wall, 8 taps cannot amortize — REJECTED
- filter shrink m=3: -17% wall but fails OTA CRC — REJECTED
- rational specialization: marginal after the fused-tap MAC — REJECTED
- Ir is not wall: the sfd clamp idea removed 7.1% Ir and LOST 20% wall
  (auto-vectorized complex MAC); every change here gates on the capture1
  paired-interleaved wall A/B, not Ir

## Playbook (unchanged)

Profiling = attribution, never the verdict:

    make PROFILE=1
    valgrind --tool=callgrind --callgrind-out-file=data/prof/<tag>-callgrind.out \
      bin/file_lora.prof --conj -w 0x2b --input-rate 1024e3 --bw 250e3 \
      data/capture1/all_frames.cf32
    python3 hotspots.py data/prof/<tag>-callgrind.out

Quality verdict for anything touching the signal path: regression +
the resampler-in-path BER sweep (--file-rate 250000 --cfo-max 32,
shared seeds, diff of --verdicts files = flip count). Perf verdict:
paired-interleaved wall on capture1 against a fresh-main worktree.

## Ideas — small, in attack order

1. **Hoist the per-sample state churn out of the hot loop.** The drained
   check (26.4M) runs per output but is a one-shot per stream start; the
   ring wrap (27.7M) disappears with a power-of-two ring + mask or by
   processing in blocks where `oldest` is carried in a register. The
   batch shape also gives the compiler contiguous pointers to reason
   about. Mostly bit-exact (watch float order in the MAC), cheap to try,
   removes ~50M Ir of overhead that is not math.
2. **Unroll h_sub to a compile-time constant.** The tap loop (45.0M) is
   a runtime bound; making the 8-tap branch count a constant (per-branch
   specialization at resampler creation) lets the compiler keep the
   accumulator in registers and drop the induction. Bit-exact. Small.
3. **Coefficient phase table.** Replace per-tap `h0 + mu*(h1-h0)`
   (69.3M = 8.6% of the WHOLE profile) with a precomputed table over a
   mu grid (e.g. 64-256 phases x h_sub taps, built at create). One table
   row fetch per output replaces 8 interpolations. NOT bit-exact (mu is
   quantized) -> quality-gated; capture1's ratio is rational (125/256),
   so on the bench a 256-phase table is EXACT - the sweep decides what
   the irrational real-world cases tolerate. Watch table size vs cache.
   MEASURED 2026-11-02 (after 1+2 landed), REJECTED: 64-phase table,
   Ir 738.8M -> 711.1M (-3.8%) but wall FLAT (73.0 vs 72.9 ms paired);
   after the idea-2 unroll the interpolation is 8 scalar FMAs on
   registers, and the table swap trades them for a quantize + a second
   128KB working set. Not worth the code path at zero wall gain.
4. **Cycles, not instructions, steer round two.** Before any big
   rewrite, re-profile wall attribution (perf stat / callgrind cycles
   event): 8 taps x 8B complex = 64B of ring reads per output is a cache
   line per output; if stall-bound, the levers are data layout (SoA,
   ring locality) not instruction count. Cheap, prevents building the
   wrong thing. MEASURED 2026-11-02: cachegrind (perf not installed) -
   LL misses ~0 program-wide, resampler D1 miss rate trivial (458k of
   4.5M). NOT memory-bound: instruction count is the lever, which picks
   5 over 6. (Hardware-only effects - branch mispredicts, uop limits -
   remain invisible to this instrument.)

## Ideas — brave

5. **Two-stage: half-band + rational corrector.** The architectural
   lever the previous campaign deferred (est. 15-18% wall). Stage 1: a
   fixed 2:1 half-band (constant coefficients, no mu, ~7-15 taps, zeros
   every other tap - the sfd half-band result says the gate tolerates
   this class of filter). Stage 2: a SHORT rational/interpolating
   corrector for the residual ratio (125/128 after the halving), where
   fewer taps x fewer outputs x SIMD-viable constant-coeff rows add up
   differently than 8 x everything. Restructure, not a patch; new plan
   of record if it opens.
6. **Decimate-by-2 fast path.** When the current ratio is within
   tolerance of exactly 1/2 (true for the standard 2x-oversampled
   capture family), bypass the general polyphase engine entirely: pure
   half-band, no mu, no coefficient interpolation. The general engine
   stays for arbitrary ratios. Quality-gated; the bench ratio 125/256
   is within 0.4% of 1/2 - decide via the BER sweep whether the 0.4%
   rate error (a slow SFO-class drift the sync already tracks) is
   tolerable, or whether the path only fires on exact ratios.
7. **Overlap-add FFT filtering.** For large-N filters FFT convolution
   wins; at 8 taps it cannot. Listed to record why it is rejected
   without a run: the filter is short, the win regime is >=32 taps.

## To-explore, not to-do (external feedback, proof-read, unverified here)

Side channel raised after the sfd half-band landed. The half-band
sparsity is intrinsically a divide-by-2 phenomenon (sinc(n/2) zeros at
every even offset); at c=1/M only ~1/M of taps vanish, so os=3/5 have no
structural shortcut, and os=4 cascades two half-bands. Items, none
measurement-backed in this repo yet:

- **Equiripple (Parks-McClellan) redesign of the generic 65-tap path**
  (~2x fewer taps at the same specs, standard SDR practice). Caveat: the
  generic path serves os=4/8 only - capture1 and the perf gate run os=2,
  so the win is invisible to the current bench; an os=4 bench case would
  have to exist first.
- **Equiripple half-band at the same length (15 taps).** Same speed,
  more stopband - OTA robustness margin against out-of-band
  interferers, not a perf item. AWGN sweeps cannot see the difference;
  only real captures could.
- Numeric nit worth keeping: at N=15 the half-band has 9 non-zero taps
  (odd offsets +-1,3,5,7 plus center), not "half".

## Plan of record

1 -> 2 (one bit-exact commit each, wall-gated), then 3 (quality-gated),
4 before deciding between 5 and 6. 7 is closed. Expected size: 1+2 trim
~8-10% of total Ir with little wall risk; 3 removes up to 8.6% total Ir
and is the first quality-gated step; 5/6 are the structural endgame.
