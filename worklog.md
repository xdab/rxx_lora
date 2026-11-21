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

### Decisions on file

- `decisions.md` — items pending the owner: source-tarball vs auto-snapshot,
  tarball linkage (bookworm-dynamic vs musl-static), version stamping inside
  binaries (needs a code change; current constraint is workflows-only).
