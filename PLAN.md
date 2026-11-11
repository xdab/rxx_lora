# PLAN.md — the simplification campaign

Shrink the codebase by deleting what cannot fire and rewriting what custom
code does no better than the textbook form, while keeping the core
capabilities intact. This document is the campaign's working ledger: what the
targets are, what was found, and what still has to be verified before it
ships.

## Mission and rules

- **Trade:** a measured loss of up to ~0.2 dB at the PER knee is acceptable
  for a net ~1000 LoC. Everything below the floor ships BER-neutral.
- **Floor:** the receiver stays above ~10x real time; the performance gains
  of the earlier campaigns are not for sale. A rewrite that adds wall clock
  must buy a large LoC savings to be considered.
- **Verification is per-batch.** Every batch runs the gates (`regression.py`
  41/41, `make tidy`, `make fmt-check`, clean `make`) and a paired-seed
  BER A/B on MediumFast (SF9/250k, CR 4/5, netid 0x2b) — protocol in the
  `ber-sweep` skill: identical seeds and grid both arms, judge on the
  `--verdicts` diff before the plot.
- **"Never fires" is a claim, not an observation.** Each deletion cites why
  it cannot fire (construction, call graph, config space) or is moved to
  Tier 3 for a measurement.
- Two categories, both count toward the total: **delete** (dead code) and
  **rewrite** (custom → textbook form).

## Instruments

- `make cov` — `-O0 --coverage` build of `bin/file_lora.cov` (added by this
  campaign). Run the full regression plus adversarial flag exercises
  (`--chase`, `--report-bad`, `-o 1/4/8`, `--conj`, `--dc-block`,
  `-s 7/11/12`, `-p 16 -a 8`), then `gcov -o obj/cov -b src/*.c` from the
  repo root. Every batch re-runs it: no line that was covered before a batch
  may be uncovered after it unless the code was deleted.
- Baseline: 7,536 LoC (C, src+include); 248 unexecuted lines of ~1,870
  executable under the full corpus. `hamming`, `utilities`, `header`, `crc`,
  `deinterleave`, `dewhiten` are 100% covered.
- BER A/Bs live in `/tmp/ber` as saved `.log`/`.v` pairs; the base arm of
  batch 1 doubles as the base arm of every later batch.

## Batch 1 — dead by construction (delete outright)

All BER-neutral expected; the batch verdict is a bit-exact regression and a
flat BER diff.

| Item | Where | LoC |
|---|---|---|
| Generic resampler kernels + dispatch indirection (`h_sub` is always 8; only the compiled-out `RESAMP_M` env knob could change it) | `src/resample.c` | ~45 |
| `RESAMP_M/BW/AS/NPFB` experimental knob block | `src/resample.c` | ~16 |
| Dead `sfd_result` fields (`q,u,d,tau,f,lam,margin` — consumer reads only `t_edge`,`t_frac`) | `src/frame_sync_internal.h`, `src/sfd.c` | ~12 |
| Dead phase guards and the unreachable `default:` arm in the verdict switch | `src/detect.c`, `src/sync.c` | ~18 |
| Dead `r->sf` field; trace-only `grid_shift` block | `src/receiver.c` | ~16 |
| `st` snapshot out-param threading (8 sites; caller re-reads status on demand) | `src/detect.c`, `src/sync.c`, `src/frame_sync.c` | ~12 |
| Dead `'h'` exit-0 ternary (getopt never returns `'h'`) | `src/main_*.c` | ~3 |
| Ring duplicate `struct ring;` declaration | `include/ring.h` | 1 |
| `output_frame` null guard (sole caller always passes a struct) | `src/output.c` | ~2 |
| `nearest_gain` clamp fix (clamp result discarded by re-query; latent overflow) | `src/device_rtl.c` | 2, bug fix |
| Per-symbol trace scaffolding with static counters | `src/demod.c`, `src/receiver.c` | ~30 |

Moved out of the batch on evidence: the ring `chunk?:1`/`cap?:4096`/cap==0
defer guards (the cap=0 deferral is documented API in ring.h, not dead code)
and the sync verdict switch's `default:` arm (`bugprone-switch-missing-default-case`
is a tidy gate; the arm now carries no message, only `break`).

## Batch 2 — dedup and textbook rewrites

BER-neutral expected; each item is one commit, gates green.

| Item | LoC |
|---|---|
| Shared live-main header (`main_rtl.c` ≈ `main_rsp.c` ~85% identical; usage text and device-specific options stay per-binary) | ~360 |
| Shared device-stream header (`device_rtl.c`/`device_rsp.c`: read, health report, `mono_seconds`) + one `regression_perf.py` run | ~120 |
| `frame_sync_create/cleanup`: 26 mallocs + 27-term NULL check → table-driven alloc/free | ~45 |
| CRC-16 table → 8-line bitwise loop (once per frame, off the hot path) | ~24 |
| `logging.c` five identical va_list stanzas → one macro | ~30 |
| `bessel_i0` written twice + Kaiser-sinc pattern duplicated → shared `dsp` helper | ~20 |
| JSON record trimmed to fields any consumer reads; cascade deletes `source_input_sample`/`source_time_seconds` | ~55 |
| `cfo_correc_aug` scratch array → loop fusion (deletes the largest per-frame buffer) | ~14 |
| `parse_duration` ×2 → `main.h`; chirp-builder dead `os_factor` param; `util_most_frequent` → `detect.c`; source conj/dc helper; `sto_frac` pad loop | ~60 |

## Batch 3 — measured rewrites (one experiment each)

| Item | Experiment |
|---|---|
| `sfd_slot_dec` SIMD kernel → the scalar FIR the edge loops use (~55 LoC) | callgrind + wall A/B; accept only wall-neutral |
| `sfd_lock`/`sfd_coh_pow` merge (~35 LoC) | paired-seed BER A/B (fire-time machinery) |
| Chase-II retirement (~200 LoC; nothing in the repo enables `--chase`) | **owner decision** after a BER sweep with/without `--chase` at the knee; sensitivity feature, not dead code |
| `--chase-k/--chase-tau` knobs (~35 LoC; always at defaults everywhere) | follows the chase verdict |
| `REWIND_SKIP` env knob → constant (~12 LoC) | none (default builds identical) |
| `RTL_CFO_ADD`/`RTL_SFO_SET`/`FRAC_M` knobs (~30 LoC) | none if the sweeps that used them are concluded |

## Flagged, not candidates (verified alive — do not touch)

The integer gather fast path (the actual hot path), the two-branch output
form in the resampler (the boundary genuinely needs it), the accept-any
netid branch (real monitoring capability), the net-id joint arbitration
(documented real-capture rationale), the rewind mechanism itself (keep the
behavior, only the knob is a candidate), `hamming.c`'s alt/margin out-params
(chase consumes them — verdict pending Tier 3).

## Deferrals

Python tools (3,123 LoC) sweep after the C code settles — the tools assert
behavior of a binary that batches 1–3 change.

## Needs attention (owner decisions)

- `main_rtl.c`: usage text says `-o` default 2, code sets 4. Preserved as
  found (behavior change deferred); pick one — text or code — from the
  verified live configuration.
- Chase-II: the Tier-3 verdict (delete vs keep vs disable-when-off) is
  pending its BER sweep; `hamming.c`'s alt/margin out-params hang on it.

## A/B protocol for the batches

The base arm of every A/B is a build of `HEAD` in a clean worktree (never
the working tree's binary), e.g. `git worktree add /tmp/lora-head HEAD &&
make -C /tmp/lora-head file`. Both arms run the identical MediumFast sweep
(preset above, 250 trials, `--verdicts`), and the verdict is the
`--verdicts` diff before any plot.
