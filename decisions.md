# Decisions

Questions reserved for the project's primary architect. Each entry: question,
options, recommendation, context — enough to decide cold. Recorded, not made.

## 1. Custom source tarball vs GitHub's auto snapshots — RESOLVED 2026-11-21

GitHub auto-generates "Source code (zip)/(tar.gz)" for every tag; the workflow
also built a custom `rxx_lora-<ver>.tar.gz` (version dir, VERSION file, tested
by the gate). The owner resolved it: **drop the custom tarball from the
release** — same tagged content, don't occupy asset space. The gate still
builds the archive (insurance that the tagged tree is complete), but the
publish job excludes `source-assets` and the published custom tarball was
deleted. Auto snapshots are the canonical source download.

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

## 3. Version stamping inside binaries — DEFERRED by owner 2026-11-21

Binaries don't know their version; the proper fix (`-DVERSION` from git
describe / the tarball's VERSION file in the Makefile + a usage line) is a
code change. The owner took it off the backlog: "up to the maintainer, that's
me, to manage." Not planned for agent work unless the owner reopens it.

## 4. When to publish the draft releases — RESOLVED 2026-11-21

Originally: drafts only, publishing stays a human click. The owner resolved it
mid-run: "publish the Linux assets; amend later with Windows binaries" —
confirmed that published releases can take added assets later. `v1.0.0` was
published with the five Linux assets; Windows zips are amended onto it by the
publish job on the next tag run. For FUTURE tags the draft-first flow
continues unless the owner says otherwise.

## 5. Windows 32-bit arm — RESOLVED 2026-11-21: build it

MSYS2's i686 repo no longer packages `mingw-w64-i686-fftw` /
`mingw-w64-i686-rtl-sdr` ("target not found", run 44902315387). The agent
recommended dropping 32-bit; the owner overruled: XP/7-era users should not be
locked out. So the release workflow builds the missing deps from source into
the i686 toolchain prefix (`--prefix=/mingw32`: FFTW 3.3.10, libusb 1.0.27,
rtl-sdr from osmocom), then builds the receiver exactly like the 64-bit arm.
Consequence: the win32 arm carries a from-source dependency build that can
drift (rtl-sdr tracks osmocom's default branch) — if it breaks, that job fails
first and should be checked against upstream releases.

## 6. Minor naming choices — RESOLVED 2026-11-21

- RPM dist tag: keep `1.fc45`-style (`%{?dist}`) and keep `fedora:latest`
  (owner: "latest is the best").
- Source tarball vs auto snapshots: see #1 (auto snapshots win).
