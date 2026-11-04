---
name: profiling
description: Profile the receiver with valgrind + callgrind — function/line/call-site Ir attribution, cache-miss views, and the wall-clock twin. Use when the user asks where time or instructions go, why something is slow, wants a profile or hotspots, names callgrind/valgrind/Ir/RTx, or claims an optimization — even if they just say "profile it" or "what's eating the cycles".
---

# Profiling

Every performance verdict rests on **two metrics, both measured**:

- **Ir** — instruction counts from callgrind. Deterministic for a given binary
  + input, so two runs compare exactly and attribution goes down to a line.
    This is the _finder_.
- **Wall clock** — seconds, from `tools/regression_perf.py` (RTx) or plain
  `time` on the release binary. This is the _confirmer_: the user feels
  seconds, not instructions, and an Ir win can vanish in cache or branch
  effects.

A change is only real when both move. The repo's own optimization commits
report both numbers for exactly this reason.

## The binary

`make profile` builds `bin/*.prof` with the production codegen (`-O3
-march=native`) plus `-g -fno-omit-frame-pointer` in a separate `obj/prof` —
so callgrind gets symbols while the instructions stay the release
instructions. Never profile (or time) a `-O0`/`-Og` build: its hot spots are
the optimizer's job, not the code's.

Always build with `-B`: a stale `obj/prof` relinks without debug info and
every function shows as `???`.

## Quick start

```bash
make profile -B
valgrind --tool=callgrind --callgrind-out-file=/tmp/cg.out --cache-sim=yes \
  ./bin/file_lora.prof -s9 -o2 <decode flags as usual> <capture.cf32>
python3 tools/prof/hotspots.py /tmp/cg.out
callgrind_annotate --tree=caller --threshold=95 /tmp/cg.out | less
```

Any cf32 capture works — a `data/capture*` frame or a `tools/frame.py` build.
Callgrind runs ~50× slower than native, so prefer short captures; the Ir
counts are exact regardless of length.

## The two views

Both read the same `callgrind.out`; cross-check them when in doubt.

**`tools/prof/hotspots.py`** — the compact view and the default deliverable.
Its functions table is the answer to "where does it go":

```
function                            Ir       Ir %
src/sfd.c:sfd_slot_dec          8.942M   34.47%
```

Flags: `--top N` functions shown, `--lines N` hot source lines per file
(with the source text), `--calls N` call sites with inclusive cost and call
counts, `--sort EVENT` to re-rank by any recorded event (`D1mr` cache-read
misses, `D1mw`, `DLmr`), `--callers NAME` for who calls a function,
`--selftest` to verify the parser.

**`callgrind_annotate`** — the same data as a full annotated source listing.
`--auto=yes` (default) annotates every source file past the threshold — that
is the line-by-line attribution. `--tree=caller|callee` prints who calls /
what is called per function, the fast way to separate per-run cost from
one-time init. `--threshold=95` trims the tail; `--context=N` widens the
source window.

## Reading a profile

1. **Totals.** hotspots.py's first line — sanity-check the Ir total against
   the input size before trusting anything below it.
2. **Functions (self).** The top of the self table is where the cycles go.
   Group entries by the code they belong to; `[bracketed]` names tag
   symbol-only frames with their `.so`.
3. **Call sites (inclusive).** For each hot function, find who pays. A
   function called `1x` from a `*_create()` amortises away on a long stream;
   one called thousands of times from the hot loop is the real target.
4. **Re-sort by cache misses.** `--sort D1mr --lines 0 --calls 0` — a
   compute-bound function with high `D1mr` is where a data-layout change
   pays. Repeat with `D1mw`, then `DLmr`.
5. **Hot lines.** `--lines N` prints the hottest statements per file with
   their source text — usually the one line to change.
6. **Attribute.** `--callers NAME` or `--tree=caller` to see whether the
   caller is your code, libm, or a library kernel.

Conventions that keep the reading honest:

- **Self vs inclusive.** Self = cost inside the function; inclusive adds
  everything it calls. Sum the self column, never the inclusive one.
- **Recursion double-counts inclusive cost** — trust the call-site table for
  recursive functions.
- **`???` with no name** means the profile objects went stale: `make profile
  -B`. A named `???` (`libfftw3f`, libm) is just a library without debug info
  and is attributed fine.
- **Ir is not seconds.** Cache-sim events are counts too. Wall clock has its
  own section below.

## Wall clock

Measure on the release binary from plain `make` (the `.prof` binary shares
its codegen and is also fine — just never a debug build). Paired runs on the
identical input, same flags:

```bash
python3 tools/regression_perf.py     # RTx per case, the repo's speed gate
time ./bin/file_lora <flags> capture.cf32 > /dev/null
```

Quote both numbers in any performance claim — e.g. "−42% wall clock, −49% Ir"
— because neither alone proves the other.

## Campaigns

An optimization campaign is a chain of paired A/Bs: one logical change per
commit, and the artifacts branded with the commit they belong to —

```sh
d=/tmp/prof/$(git rev-parse --short HEAD); mkdir -p "$d"
cp bin/file_lora.prof "$d"/ && valgrind --tool=callgrind \
  --callgrind-out-file="$d/cg.out" ./bin/file_lora.prof ...
```

(reusing the quick-start flags). An old profile stays exactly valid for the
binary that produced it, so comparing two commits is just two `hotspots.py`
runs on two archived outs — the hash names every arm.

## Gotchas

- One-time init (resampler/filter design) over-weights short captures;
  compare captures of similar length or ignore `*_create` call sites when
  extrapolating to streaming.
- `--cache-sim=yes` adds the miss events; without it only Ir/Dr/Dw exist.
- Attribution moves with the binary: a current claim needs a current profile,
  which is why archived outs are branded per commit (Campaigns, above) —
  never re-read an old profile as if it described today's binary.
