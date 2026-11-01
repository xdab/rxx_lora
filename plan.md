# plan.md — `performance` branch: memory-for-CPU port + callgrind profiling

Goal: re-apply the seven memory-for-CPU receiver changes from the retired
`prof/mem-cpu` branch (base `aa5d575`, now 3 commits behind `main`) onto fresh
`main`, one commit per change, with a callgrind baseline + per-patch profile
recorded for later analysis. Work continues on another machine, so this file,
the raw patches and the raw profiler outputs travel in the repo / `data/`.

## Inputs

- The retired round's raw patches (a `git format-patch` export of
  `aa5d575..prof/mem-cpu`, renamed to the original commit order) drove the
  porting; they are NOT `git am`-able — main's #4 (receiver depth) rewrote
  `src/sfd.c`, `src/sync.c`, `src/receiver.c` and touched `src/demod.c`,
  `src/hamming.c`, `src/frame_sync_internal.h` — so they were ported by hand,
  literally where context matched, in spirit where it didn't. The patch files
  lived under `patches/` during the port and were dropped once all seven had
  landed.
- `tools/hotspots.py` — callgrind.out hot-spot attribution (function self-cost,
  hot lines, call sites, `--callers`); copied from the old Forgejo repo's
  `tools/prof/`, self-contained, `--selftest` included.
- Numbers/measurement discipline from the retired round: `data/TODOS-MEM.md`
  (gitignored, machine-local) carries the paired-run measurements and the
  house rule: LUTs preferred over CPU implementations regardless of measured
  size — small wins are kept.

## Patch order (one commit each)

1. hamming: decode-side ±1 sign LUT (640 B static table, dot-product scan)
2. resampler: circular delay line (O(1) push; increment-with-wrap, no mask —
   `h_sub` is only a power of two for some experimental RESAMP_M values)
3. sfd: power spectra end to end, no `hypotf`/`sqrtf` (mag² is scale-equivalent
   for every consumer)
4. demod: one fused spectrum pass per window (peak, runner-up, blanked noise
   floor cached in `demod_state`); noise sum accumulates non-blanked bins
   directly — sum-all-then-subtract cancels catastrophically in float32
5. detect: score the sliding sums directly, divide only at the trace print;
   drop the `detect_mag_avg` buffer (one full N-pass less per DETECT window)
6. dsp: FFTW_MEASURE plans with a wisdom cache
   (`${XDG_CACHE_HOME:-~/.cache}/rxx_lora/fftwf.wisdom`; export at EVERY alloc
   or only the first size lands in the file); short runs pay a ~3 ms warm floor
7. sfd: compute each slot decimation and pair spectrum once (partial take from
   the retired round: coarse pair-slot spectra cached, fine pass decimates the
   12 unique offsets once; the full one-shot span decimation stays open)

## Profiling loop (per commit)

- Build: `PROFILE=1 make` → `bin/*.prof` (`-O3 -march=native -g
  -fno-omit-frame-pointer`, separate `obj/prof/`).
- Baseline + after each patch:

      valgrind --tool=callgrind \
        --callgrind-out-file=data/prof/<short-hash>-callgrind.out \
        bin/file_lora.prof --conj -w 0x2b --input-rate 1024e3 --bw 250e3 \
        data/capture1/all_frames.cf32

  capture1: 9 frames, 28 MB, ~3.4 s of IQ at 1.024 MHz — small enough for
  valgrind's ~20–100x slowdown, large enough to hit many DETECT fires.
  capture1/2 are spectrally mirrored (`--conj`), capture3 is not; all at
  1.024 MHz (capture4 1.000 MHz). The 1.024 MHz input exercises the resampler,
  which the perf matrix never does — the resampler patch finally gets measured.
- Attribution: `python3 tools/hotspots.py data/prof/<...>.out`.
- Summary results recorded in `data/PROF.md` (gitignored): per-commit table of
  hot functions and deltas. Raw outputs stay in `data/prof/`.
- Wall-clock verdict per commit: `regression_perf.py` paired runs (callgrind
  says where; wall clock says whether it paid).

## Gates per commit

clean `make` under `-Wall -Wextra`; `clang-format -i` on touched files;
`make tidy`; `python3 tools/regression.py` fully passing; Python: `ruff
format`/`ruff check` on touched tools. Regressions are the accept/reject gate;
the profiler is attribution, not the verdict.

## Conventions

- Every patch commit message: `<module>: <what>` matching repo style.
- Profiler artifacts never committed (`data/` gitignored; repo `.gitignore`
  already excludes `callgrind.out*`).
- A parallel agent works on `tools/ber_readme.py` + docs/plots — never stage or
  kill anything of theirs.
- FFTW wisdom patch note: warm-cache benefit is machine-local; the committed
  code is the cache mechanism, not any wisdom file.

## Status

- [x] branch `performance` off `main` (2340a49)
- [x] raw patches imported under `patches/` (dropped again once the port
      completed — the commits are the record)
- [x] `tools/hotspots.py` copied, selftest + ruff clean
- [x] baseline callgrind profile (1,353.8M Ir; machine-local numbers in `data/PROF.md`)
- [x] patches 1–7 ported, profiled, gated (one commit each, f7df25e..85631eb)
- [x] wrap-up: `data/PROF.md` summary, final confirmation on capture3 (49/49 CRC valid
      at every gate run). End-to-end paired wall clock: branch ~22-25% faster on
      capture1; perf matrix 26-27/27 cases faster, 0 slower.
