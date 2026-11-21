# Decisions

Questions reserved for the project's primary architect. Each entry: question,
options, recommendation, context — enough to decide cold. Recorded, not made.

## 1. Keep the custom source tarball, or rely on GitHub's auto snapshots?

Every release/tag page auto-generates "Source code (zip)" and
"Source code (tar.gz)" from the tag — that part can't be removed. The release
workflow additionally builds its own `rxx_lora-<ver>.tar.gz`.

- **Keep both (recommended)** — the custom tarball is the one the gate tests
  (extract → `make file` → `tools/regression.py --no-capture`), it carries the
  release version in a `VERSION` file (which a future Makefile version stamp
  reads), consistent naming `rxx_lora-1.0.0.tar.gz`, clean versioned top dir.
  Auto snapshots stay as untested extras.
- **Drop the custom one** — publish job stops uploading `*.tar.gz`; the gate
  loses its "archive completeness" meaning (it would test a file we don't
  ship). ~10 workflow lines saved.

Context: raised in session; user asked "we don't have to duplicate this,
right?" — recommendation given, no verdict yet. Nothing blocks on it; current
state ships both.

## 2. Linux binary tarball: linkage flavor

The goal adds a Linux binaries tarball. Shipped binaries can't use
`-march=native` (runner CPU ≠ user CPU) — the workflow already overrides
CFLAGS per-build (`make CFLAGS=...`), no Makefile change needed. The open
question is linkage:

- **Dynamic, built on debian:12 (recommended, proceeding for now)** — glibc is
  backward-compatible: bookworm's glibc (2.36) runs on anything newer; matches
  the proven CI distro matrix and the deb. Downside: won't run on older
  LTS glibc (ubuntu 20.04 and older, debian 11).
- **Static musl (alpine container)** — one binary per arch, runs anywhere
  Linux, no glibc floor. Downside: different toolchain than everything else;
  first time alpine is used for anything but a compile check in this repo.

Decision needed only if/when users report the bookworm floor is too high;
builds proceed on the debian:12-dynamic arm meanwhile.

## 3. Version stamping inside binaries (`--version` / usage line)

Binaries currently don't know their version. Proper fix: `-DVERSION` from
`git describe`/the tarball's `VERSION` file in the Makefile + a line in each
binary's usage. **Blocked on constraint**: the standing instruction for this
effort is workflows-only (no code changes). Recorded so the next code-touching
session picks it up; also the reason shipped deb/rpm binaries can't
self-identify yet.

## 4. When to publish the draft releases

The pipeline attaches everything to DRAFT releases (only the owner sees them;
asset URLs become semi-permanent once users depend on them). Publishing
`v1.0.0` (and later tags) is a human click on
https://github.com/xdab/rxx_lora/releases — deliberate: not delegated to the
autonomous run.
