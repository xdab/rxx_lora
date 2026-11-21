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

## 4. When to publish the draft releases — RESOLVED 2026-11-21

Originally: drafts only, publishing stays a human click. The owner resolved it
mid-run: "publish the Linux assets; amend later with Windows binaries" —
confirmed that published releases can take added assets later. `v1.0.0` was
published with the five Linux assets; Windows zips are amended onto it by the
publish job on the next tag run. For FUTURE tags the draft-first flow
continues unless the owner says otherwise.

## 5. Windows 32-bit arm — build it at all?

**Blocked fact**: MSYS2's i686 repo no longer packages `mingw-w64-i686-fftw`
nor `mingw-w64-i686-rtl-sdr` ("target not found", run 44902315387). The
project's own toolchain path cannot produce a 32-bit build. Remaining options:

- **Drop the 32-bit arm (recommended)** — 64-bit is the primary goal; a 32-bit
  build would require cross-building FFTW + libusb + librtlsdr from source for
  i686-mingw (or pinning dead package mirrors), a permanent maintenance burden
  for a legacy audience. The 32-bit job was removed from the workflow.
- **From-source i686 dependency farm** — a Linux job cross-compiles the three
  deps then the receiver; roughly +100 lines of workflow and ongoing upkeep.

Context: goal phrasing was "Windows 32-bit if possible" — it is not, from
packaged toolchains. Nothing else in the pipeline waits on this.

## 6. Minor naming choices (batch, decide anytime)

- **RPM dist tag**: `Release: 1%{?dist}` yields `1.fc45` (the Fedora release
  built on). Keep (standard, informative) vs drop (`1.x86_64.rpm`). Also note
  `fedora:latest` floats — pin a versioned Fedora if the tag must be stable.
- **Source tarball vs auto snapshots**: see #1.
