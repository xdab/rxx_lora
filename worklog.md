# Worklog

Captain's logbook for autonomous runs. Newest efforts on top; each effort owns
a section. Written as events happen, not reconstructed later.

## CD bring-up: full release asset pipeline (2026-11-21, autonomous run)

Goal: Linux binary tarball + Debian .deb + Fedora RPM publishing, then Windows
64-bit (exe+dlls) primarily and 32-bit if possible — all as release assets.

Context at handoff: source-tarball stage already live (`.github/workflows/release.yml`,
draft release `v1.0.0` exists with `rxx_lora-1.0.0.tar.gz` + `SHA256SUMS`; .deb
job built and green in dry runs). Earlier in the day (user-driven): workflow
created, tag `v1.0.0` cut, deb added — three fixes found via CI logs:
container jobs default to `sh` and git refuses the runner-owned checkout
(version step), Debian package names disallow underscores (`rxx-lora`),
`gh` in a job without checkout can't infer the repo (`GH_REPO` env).

### 2026-11-21 11:50 — publish job: `--clobber` after `--` is a shell glob, not a flag

Run 44900915952 (tag push, all builds green): publish failed with
`no matches found for '--clobber'`. I had written
`gh release upload "$tag" -- *.tar.gz *.deb SHA256SUMS --clobber` — the flag
after `--` gets glob-matched. Fix: no `--`, flag before files, `shell: bash`
pinned. Lesson for this runner image (ubuntu26): don't place flags after `--`,
pin the shell when relying on bash semantics.

### 2026-11-21 11:55 — milestone: source tarball + .deb live on the v1.0.0 draft

Run 44901298139 all green; draft release assets now: `rxx_lora-1.0.0.tar.gz`
(452 KB), `rxx-lora_1.0.0_amd64.deb` (61 KB, stripped), `SHA256SUMS` over both.
The deb was validated against bookworm's own index (`apt-get install --dry-run`
resolves Depends: libfftw3-single3, librtlsdr0). The .deb binaries are built
with portable CFLAGS (`make CFLAGS=...` command-line override — no Makefile
change, honoring the workflows-only constraint).

### 2026-11-21 12:00 — milestone: Linux bin tarball + RPM added; 5 assets on the draft

The Linux binary tarball reuses the deb job's stripped portable bookworm build
(same binaries, one toolchain — no duplicated compile). RPM: `fedora:latest`
container, prebuilt-binaries spec, **AutoReqProv on** so rpmbuild derives
`libfftw3f.so.3`/`librtlsdr.so.0` deps from SONAMEs instead of guessing Fedora
package names; `dnf install` of the produced rpm validates them for real.
First rpm attempt failed twice (docs not copied into `rpmbuild/SOURCES`;
Fedora's `source_date_epoch_from_changelog` demands a `%changelog` entry).
Two fixes → green. User-visible note: they initially looked at
`/releases/tag/v1.0.0`, which never shows draft assets — the draft lives
under Releases with a Draft badge (publishing stays the owner's click).

### 2026-11-21 12:06 — v1.0.0 PUBLISHED with the Linux assets

Win64 build came up first try (UCRT64, staged DLLs, regression gate green in
dry run 44902036482). Owner check-in mid-run: publish the Linux assets now,
amend Windows later — confirmed amended assets on published releases are fine,
so `gh release edit v1.0.0 --draft=false`. Five assets live (source tarball,
Linux x86_64 binaries tarball, deb, rpm, SHA256SUMS); win64 zip lands via the
publish job on the next tag run. decisions.md #4 updated to resolved.

### 2026-11-21 12:08 — win32 verdict: not possible from packaged toolchains

`mingw-w64-i686-fftw` and `mingw-w64-i686-rtl-sdr` are gone from MSYS2's
mingw32 repo (run 44902315387: "target not found"). The goal said "32-bit if
possible" — from packaged toolchains it is not. The only route is a from-source
i686 dependency farm (fftw+libusb+librtlsdr), which is a real maintenance
burden for a legacy arm → recorded as decisions.md #5 with drop-it as the
recommendation; the permanently-failing job was removed from the workflow so
the release flow stays green. Final tag run next: amends the published v1.0.0
with the win64 zip + refreshed SHA256SUMS.

### 2026-11-21 12:15 — all six assets on the published v1.0.0; owner decisions round

Final tag run 44902593149 all green: published release now carries the win64
zip (2.1 MB, self-contained exes+dlls) + refreshed SHA256SUMS alongside deb,
rpm, linux-x86_64 tarball. Owner then ruled on the two pending questions:
drop the custom source tarball from the release (GitHub's auto snapshots are
the same tagged content — decisions.md #1 resolved; publish job excludes
source-assets, the published rxx_lora-1.0.0.tar.gz asset was deleted, the
gate stays), and keep the fc45 dist tag (decisions.md #6 half-resolved).

### 2026-11-21 12:20 — GOAL COMPLETE: all targets on the published v1.0.0

Final run 44903013127 all green. Independent verification from a fresh
download: all four assets match SHA256SUMS; win64 zip is self-contained
(file_lora.exe + rtl_lora.exe + libfftw3f-3/librtlsdr/libusb-1.0/
libwinpthread-1 DLLs + docs); linux-x86_64 tarball carries bin/ + docs.
One publish iteration lost to download-artifact@v4: its `pattern` input does
NOT support `!` exclusions — matched 0 of 5 artifacts; fixed by removing the
gate-only tarball by exact name before checksumming.

Final release shape (v1.0.0, published): deb, rpm, linux-x86_64 tarball,
win64 zip, SHA256SUMS + GitHub's auto source snapshots. 32-bit Windows: not
possible from packaged toolchains (decisions.md #5).

### Decisions on file

- `decisions.md` — items pending the owner: source-tarball vs auto-snapshot,
  tarball linkage (bookworm-dynamic vs musl-static), version stamping inside
  binaries (needs a code change; current constraint is workflows-only).
