# plan.md — post-merge profiling record (UNCOMMITTED, machine-local working notes)

Re-profile of the fresh `main` (`8a334b4`, PR #6 squash) per the established
playbook. Nothing here is committed; the raw output lives in
`data/prof/postmerge-callgrind.out` (gitignored).

## Playbook

Profiling (attribution, never the verdict):

    make PROFILE=1
    valgrind --tool=callgrind \
      --callgrind-out-file=data/prof/<tag>-callgrind.out \
      bin/file_lora.prof --conj -w 0x2b --input-rate 1024e3 --bw 250e3 \
      data/capture1/all_frames.cf32
    python3 data/hotspots.py data/prof/<tag>-callgrind.out

9/9 frames decoded, crc_ok=9. capture1: 9 OTA frames, ~3.4 s of IQ at
1.024 MHz (`--conj`, spectrally mirrored), the only bench that exercises
the resampler (rate 0.488).

A/B BER sweep (the quality verdict for anything touching the signal
path; the regression gate alone cannot see sensitivity):

    # arm A: main — VERIFY THE BASELINE FIRST (git fetch; local main was
    # once 11 commits stale and produced a wrong conclusion)
    git worktree add /tmp/rxx_main main && make -C /tmp/rxx_main
    # arm B: this branch
    make
    python3 tools/ber.py --sf 9 --bw 250000 \
      --snr-min -22 --snr-max -10 --snr-step 1 --trials 1000 \
      --bin /tmp/rxx_main/bin/file_lora \
      --file-rate 250000 --cfo-max 32 \
      --verdicts /tmp/verdicts_main.txt --out /tmp/ber_main.png \
      | tee data/ber_main.log
    python3 tools/ber.py --sf 9 --bw 250000 \
      --snr-min -22 --snr-max -10 --snr-step 1 --trials 1000 \
      --bin bin/file_lora \
      --file-rate 250000 --cfo-max 32 \
      --verdicts /tmp/verdicts_perf.txt --out /tmp/ber_perf.png \
      | tee data/ber_perf.log
    python3 tools/ber_combine.py --by-file \
      --out data/ber_main_vs_perf.png data/ber_main.log data/ber_perf.log
    # attribution: the --verdicts files align line-per-trial (same default
    # seed draws); a plain diff counts the verdict flips

Parameter notes, each earned the hard way:

- `--file-rate 250000` puts the resampler in-path (frame built at
  bw*os, decimated 2:1, the binary resamples back up) — without it the
  sweep bypasses the resampler entirely and cannot judge filter changes.
- `--cfo-max 32` caps the CFO draw at the +-BW/4 acquisition limit; the
  default 20 ppm crystal model exceeds it at bw 125k and nothing
  decodes (BER 1.0 at every point, the sweep measures nothing).
- The two arms must share the tool's default seed: draws then align
  line-per-trial, and `diff` of the --verdicts files is the flip count.
- The perf arm reproduces bit-for-bit across runs — any deviation is a
  bug in the harness, not noise.

## Result — total Ir = 1,059,557,518

Matches the final branch build (1,059,557,549; 31 Ir of run-to-run
jitter): the squash-merged main is bit-for-bit the reviewed code.
For scale: the pre-campaign baseline on the same bench was
1,353,758,997 Ir → **−21.7% total instructions**, wall clock −39%
(156.8 → 95.6 ms, paired interleaved).

## Hotspot ranking (self, by Ir)

| # | Ir | % | function | vs campaign baseline |
|---|---|---|---|---|
| 1 | 438.5M | 41.4% | resample.c:resampler_push | 676.4M → −35% (ring line, fused-tap MAC) |
| 2 | 268.8M | 25.4% | sfd.c:sfd_slot_dec | unchanged (untouched by the campaign) |
| 3 | 67.0M | 6.3% | sfd.c:sfd_shift_lam | unchanged |
| 4 | 55.2M | 5.2% | sfd.c:sfd_locate | 28.7M → 55.2M (re-index loops moved here from the coarse pair loop by the dpair cache) |
| 5 | 34.6M | 3.3% | frame_sync.c:frame_sync_frac_gather | unchanged |
| 6 | 27.3M | 2.6% | demod.c:demod_soft_llrs | 35.8M → −24% (fused pass, cached floor) |
| 7 | 19.5M | 1.8% | libfftw3f | — |
| 8 | 18.4M | 1.7% | libc memset | — |
| 9 | 16.4M | 1.6% | libfftw3f | — |
| 10 | 14.5M | 1.4% | frame_sync_get_symbol_val | unchanged |

glibc `hypotf` (27.3M, #7 in the baseline) is gone entirely.

## Interpretation

- The ranking is now two-horse: `resampler_push` (41.4%) + `sfd_slot_dec`
  (25.4%) = two-thirds of all instructions. Everything else is ≤6.3%.
- The campaign is closed on #1: measured and rejected this round —
  auto-vectorization (idea 3: +24% wall), hand-written AVX2+FMA SoA
  (idea 10: −3% wall; 8-tap branches cannot amortize SIMD), filter
  shrink m=3 (idea 9: −17% wall but fails OTA CRC; BER sweep
  data/ber_resamp_*.log), rational specialization (idea 4: marginal
  after the fused-tap MAC). Remaining levers on #1 are architectural
  (idea 7: two-stage half-band + rational corrector, est. ~15-18% wall)
  or quality-bound.
- #2 `sfd_slot_dec` is the next natural target and the playbook notes
  the lever: the 65-tap FIR pays a bounds-checked `sfd_raw` per tap
  (the check alone is 10.0% of total Ir); hoisting the retention check
  by splitting interior/boundary tap ranges is unmeasured territory.
- #3/#4 (`sfd_shift_lam`, `sfd_locate`) are the fine-scoring pair;
  their hot lines are all `sfd_raw`'s bounds check as well — the same
  lever likely serves both.

## Status

- [x] fresh main profiled, ranking recorded (this file, uncommitted)
- [ ] next campaign, if opened: `sfd_slot_dec` retention-check hoist

## Brainstorm — sfd_slot_dec (the next campaign's candidate list)

Ground truth from the profile: 268.8M self Ir, 25.4% of total. Hot lines:
the `sfd_raw` bounds check per tap (105.6M, 10.0% of the WHOLE profile),
the MAC (79.2M), the tap loop (79.2M). The same bounds check also tops
`sfd_shift_lam` (2.3%) and `sfd_locate` (2.0%). Structure: per output j
the 65-tap window [base, base+64) is CONTIGUOUS in `in` — the valid tap
range is a single interval per j, yet the check runs per tap. 65 taps
also clears the SIMD amortization bar that killed the resampler ideas
(8-tap branches could not; 65 can).

Standard, in attack order:

1. **Tap-range clamp, no per-tap branch.** For each j the in-retention
   taps are t in [max(0, back-base), min(65, fwd-base)); out-of-range
   taps contribute fir[t]*0 = 0, so they can be SKIPPED, not zero-padded.
   Two clamps per output replace 65 checks. Bit-exact.
2. **Fully-interior fast path.** When [base, base+64) fits the retention
   (the common case — slots sit well inside the (2*SYNC_LOOKAHEAD+1)*sps
   window), one compare per j selects a straight contiguous MAC with zero
   checks: `acc += fir[t] * in[base+t]`. Bit-exact, and the contiguous
   loop is what makes 3 viable.
3. **SIMD on the 65-tap MAC.** 65 taps = 8 AVX2 FMA groups + reduction;
   unlike the resampler's 8 taps, the reduction amortizes 8x. SoA staging
   of the retention window (one pass, ~9*sps complex) keeps it
   shuffle-free. Auto-vectorization may already win once 2 lands — measure
   the compiler's choice first, hand-kernel only if it pessimizes (the
   resampler lesson: auto-vec and hand-vec BOTH failed at 8 taps; 65 is a
   different regime, but Ir is not wall — gate on capture1 A/B).
4. **Unify the coarse pass's decimations.** The upow loop decimates
   offsets -4..9 (14 slots) and the dpair cache decimates 2..9 (8 slots):
   8 slots decimated twice per coarse run, although patch 07 made
   decimation ref-independent. Decimate -4..9 once (14 decs), derive both
   the down-ref (upow) and up-ref (dpair) FFT rows from the shared rows.
   Saves 8 of 22 coarse decimations, bit-exact.
5. **Half-band redesign for os=2.** SFD_FIR_TAPS=65 is a windowed sinc
   for arbitrary os; a 2:1 half-band at 60 dB needs ~15 taps and has
   zeros at every other tap (2x fewer mults on top). 65 -> 15 is ~4x on
   the whole function. Quality-gated: this filter feeds acquisition
   evidence, so the gate is regression + a resampler-in-path BER sweep
   (the idea-9 instrument, which exists now), NOT just regression —
   the resampler campaign showed the gate alone can bless quality damage.
   Per-os tap counts (taps(os)) keep os=4/8 covered.

Braver, only if the above stalls:

6. **Recursive/running decimator.** Share tap products across the os-
   overlapping output windows. Does NOT work for a general FIR (the
   coefficient at position t pairs with a different input sample per
   output) — only a CIC/IIR structure shares state, and that changes the
   passband. Listed to record why it's rejected without a run.
7. **Fold the reference into the filter.** Impossible as stated: ref
   varies per OUTPUT j (constant across t), so ref_j * (fir * in) is
   already the minimal form. Recorded so it is not re-derived.

Expected size: 1+2 remove most of the 105.6M check and trim loop
overhead (~-15-20% total Ir); 3 halves the MAC lines on top (~-8-10%);
4 is small but free (~-2%); 5 alone would be ~-15% total Ir and is the
only idea that changes quality. Plan of record: land 1+2 (one commit,
bit-exact), then 3 (measure), then 4, then decide on 5 with the BER
instrument.
