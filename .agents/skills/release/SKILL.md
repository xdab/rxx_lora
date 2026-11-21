---
name: release
description: >-
  Versioning and release mechanics for this repo — SemVer rules tuned for a
  CLI tool, the breaking-change surface, how a tag becomes a published
  release through .github/workflows/release.yml, which assets ship and how
  each is gated, and how to iterate on the pipeline safely. Use when the
  user asks to cut, bump, or version a release, wonders what version number
  a change deserves or whether a change is breaking, mentions tags, draft
  releases, release assets, the deb/rpm/win64 pipeline, or asks how
  releases are produced here.
---

# Release

A release here is **a tag, and nothing else manual**. Pushing an annotated
tag `vX.Y.Z` runs `.github/workflows/release.yml`: every asset is built,
gated in its own environment, and attached to a **draft** release for that
tag. Publishing — and writing the notes — stays with the owner. Dry runs:
`gh workflow run Release` on main produces the same artifacts as workflow
artifacts and never touches the Releases page.

## Versioning — SemVer with one decision rule

`vMAJOR.MINOR.PATCH`, v-prefixed, SemVer semantics as they apply to a CLI
tool:

| Digit | Meaning | Examples from this repo |
|---|---|---|
| MAJOR | breaks an existing user | removed/renamed flag or binary, changed flag meaning **or default value**, stdout JSONL schema change, dropped device backend or PHY combo |
| MINOR | purely additive | new flags that default off (e.g. `--chase`), new binaries, new make targets, newly supported SF/bw combinations |
| PATCH | fixes and internals, interface untouched | decoder fixes, performance work, packaging, dependency bumps, refactors |

The decision rule that settles every borderline case:

> Run the user's existing command line, unchanged. If it could produce
> different stdout, files, or device actions after the change, the change is
> MAJOR. Otherwise: MINOR for anything new and user-visible, PATCH for
> everything else.

Two subtleties spelled out so agents never guess:

- **Default-value and flag-meaning changes are MAJOR, not minor.** They pass
  the rule above — scripts that never passed the flag see different output —
  and that is how CLI projects read SemVer. The looser convention of
  "benign" behavior changes in minors (with opt-out flags) was rejected for
  this repo: the ambiguity costs more than the flexibility.
- **Byte-identical decodes are not an invariant.** A decoder fix
  legitimately changes which frames come out of the same input — still a
  PATCH. Under PATCH the *shape* of the output is invariant, not the bits.

Before 1.0 the minor digit carried breaking changes (SemVer's 0.x
convention); at 1.x the rules above are strict. Suffixes like `v1.1.0-rc1`
are allowed — GitHub marks such releases pre-release on publish, asset names
inherit the suffix, and the full pipeline runs unchanged.

Binaries do not currently self-identify (no `--version`; a maintainer
decision, deliberately deferred) — the version lives in the tag and the
release metadata.

## The public interface

Breaking means breaking *these*; anything not listed may evolve freely:

- CLI flags, their defaults, and their meanings, per binary (`file_lora`,
  `rtl_lora`, `rsp_lora`)
- the stdout frame format — the JSONL schema is a de-facto API, parsed by
  downstream tooling (`tools/mesh_rx.py` rebuilds MeshPackets from it)
- the set and names of binaries and of `make` targets
- supported PHY combinations (SF range, standard bandwidths, coding rates)
- device backends (librtlsdr, SDRplay API)

## Cutting a release

1. Land the work on `main` and run the AGENTS.md quality gates locally
   (`make`, `tools/regression.py`, `tools/regression_perf.py`, `make tidy`,
   `make fmt-check`). The pipeline re-runs the regression arm per build
   environment, but that is a gate, not a substitute.
2. Tag — the owner's call, never an agent's unprompted act:
   ```sh
   git tag -a vX.Y.Z -m "rxx_lora X.Y.Z" && git push origin vX.Y.Z
   ```
3. Watch the run (`gh run watch`), then verify from the release:
   `gh release view vX.Y.Z` for the asset set, and after a fresh
   `gh release download`, `sha256sum -c SHA256SUMS`.
4. The release is a **draft with empty notes**: the owner writes the notes
   and clicks publish.

Pipeline changes iterate the other way around: edit `release.yml` on `main`,
`gh workflow run Release`, inspect the dry-run artifacts, fix, repeat — only
then move the tag. A run against an existing tag updates that release and
`--clobber`s same-named assets, so the same tag can be re-cut during
bring-up without version churn.

## What ships

| Asset | Built on | Gate before it exists |
|---|---|---|
| `rxx-lora_X.Y.Z_amd64.deb` | debian:12 container, portable CFLAGS | build + regression in-container; Depends resolved via `apt-get install --dry-run` |
| `rxx-lora_X.Y.Z-1.fcNN.x86_64.rpm` | fedora:latest container (`%{?dist}` floats with it), portable CFLAGS | build + regression in-container; auto-derived SONAME Requires resolved by a real `dnf install` |
| `rxx_lora-X.Y.Z-linux-x86_64.tar.gz` | the deb job's stripped binaries | same build and gate — one toolchain, no duplicated compile |
| `rxx_lora-X.Y.Z-win64.zip` | windows-latest, MSYS2 UCRT64 | build + staged runtime DLLs + regression on the exe |
| `SHA256SUMS` | publish job | fresh checksums over exactly what ships |

Decisions encoded in the pipeline (rationale in git history): the custom
source archive is gate-only — GitHub's auto-generated source snapshots are
the canonical source download; no 32-bit Windows arm (MSYS2's i686 repo
dropped the fftw/rtl-sdr packages; a from-source dependency farm was tried
and dropped by owner decision); `rsp_lora` ships in nothing — the SDRplay
API is proprietary, install it from the vendor and build from source; Linux
binaries are bookworm-dynamic (glibc ≥ 2.36 floor); shipped builds drop
`-march=native` via a CFLAGS command-line override, not a Makefile change.

## Pipeline gotchas

- A tag push executes `release.yml` **as it exists in the tagged commit**.
  Land workflow changes on `main` before tagging; a re-run of an old run
  replays the old workflow.
- Container jobs run plain `sh` and git refuses the runner-owned checkout:
  no `[[ ]]`, no `git describe` there. Derive versions from
  `GITHUB_REF_NAME`/`GITHUB_SHA` with `shell: bash` pinned on the step.
- `actions/download-artifact@v4` patterns accept no `!` exclusions — a
  multiline pattern that looks like an allowlist can silently match nothing.
  Filter by deleting files by name instead.
- Never place a flag after `--` in a runner script; it gets glob-matched as
  a filename. Pin `shell: bash` wherever bash semantics matter.
- Debian package names allow only lowercase alphanumerics and `-+.`
  (`rxx-lora`, never `rxx_lora`).
