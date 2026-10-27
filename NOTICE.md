# Notices

Moonlight PS4 is an unofficial, independently developed client for streaming
games from a PC running [Sunshine](https://github.com/LizardByte/Sunshine).
It is not affiliated with, endorsed by, or maintained by Sony, the official
Moonlight project, or NVIDIA. Product names belong to their respective owners.

## Project lineage

This repository is a fork that builds on:

- [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4)
  — the base PS4 client (OpenOrbis toolchain, pkg layout, plugin sources,
  Orbis-specific patches), which does not itself carry a license file at the
  time of this fork.
- [Akryst/Moonlight-ps4](https://github.com/Akryst/Moonlight-ps4) — the
  public release line of that project (releases 01.34/01.35), which
  this tree continues as release 1.6.0.

The fork's own changes are released under the GNU GPL-3.0 (see [LICENSE](LICENSE)).
Because the client statically links GPL-3.0-licensed moonlight-common-c, the
combined work is distributed under the GPL-3.0 in any case. The maintainers of
this fork welcome an explicit license statement from the upstream author and
recommend opening an upstream issue to add one.

## Third-party components

Fetched at build time by `scripts/setup_deps.sh`, pinned in
`third_party/DEPS`, and patched for Orbis via `patches/`:

| Component | Source | License |
|-----------|--------|---------|
| moonlight-common-c (+ enet, nanors) | github.com/moonlight-stream/moonlight-common-c | GPL-3.0 (MIT for enet/nanors) |
| mbedTLS | github.com/Mbed-TLS/mbedtls | Apache-2.0 |
| Opus | github.com/xiph/opus | BSD-3-Clause |
| h264bitstream | github.com/aizvorski/h264bitstream | LGPL-2.1 |
| FFmpeg (H.264-only cross-build) | ffmpeg.org | LGPL-2.1+ |
| OpenOrbis PS4 Toolchain | github.com/OpenOrbis/OpenOrbis-PS4-Toolchain | See upstream |

Vendored in this repository:

- `vendor/stb_image.h` — stb_image v2.30, public domain / MIT
  (license embedded in the header).
- UI font: Carlito (SIL Open Font License 1.1), baked at build time by
  `scripts/generate_ui_font.py` — see `vendor/FONT-LICENSE.txt`.

## Console-specific files

- `pkg/sce_module/libc.prx`, `pkg/sce_module/libSceFios2.prx` and
  `pkg/sce_sys/about/right.sprx` are PlayStation 4 system modules, carried
  unchanged from the upstream repository's packaging layout (the standard
  practice for PS4 homebrew distribution). They are not part of the licensed
  source code and remain the property of their respective owner.
- `pkg/sce_sys/icon0.png` — Moonlight eight-segment logo, carried from
  upstream.

## Artwork

See [design/ASSETS.md](design/ASSETS.md) for the provenance of the UI artwork:
renderer-generated previews, a generated lake background asset, and one
original console capture used as a design reference. The design language is
recreated in native code; no system assets, fonts or icon files are bundled.
