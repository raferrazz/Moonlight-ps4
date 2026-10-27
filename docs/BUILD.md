# Build Moonlight PS4

Run commands from the repository root. The current package version is 01.60 (release 1.6.0).

## Windows with Ubuntu WSL

Prepare the environment once:

```powershell
wsl -d Ubuntu -- bash scripts/bootstrap_ubuntu.sh
```

Build and export the package (local host tests run if present):

```powershell
wsl -d Ubuntu -- bash scripts/build_from_windows.sh
```

Output: `dist/Moonlight-PS4-01.60.pkg` and `dist/SHA256SUMS`.
The script mirrors source to `~/ps4dev/moonlight-client` to keep dependencies
and build outputs on the Linux filesystem. At the repository root, `build.bat`
invokes the same build.

## Ubuntu / Linux

```bash
bash scripts/bootstrap_ubuntu.sh
bash scripts/setup_deps.sh
bash scripts/build_ffmpeg_ps4.sh
bash tests/unit/run_all.sh      # host unit suites
bash tests/host/build.sh       # optional: UI render harness (needs Pillow)
bash scripts/build_pkg.sh
```

Output: `build-ps4/Moonlight-01.60.pkg`.
The scripts prepare OpenOrbis 0.5.4, host LLVM/Clang tools and PS4 FFmpeg.
Dependencies are pinned in `third_party/DEPS`; local PS4 changes are reapplied
from `patches/`. Do not commit patched dependency working trees into submodules.

## Checks and previews

Host tests cover the quick menu, touchpad gestures, Wake-on-LAN, the saved-PC
config list and the full UI render harness. C checks use address and
undefined-behavior sanitizers. Run `bash tests/unit/run_all.sh` after the
dependencies are prepared; the optional UI harness (`bash tests/host/build.sh`)
additionally needs Python with Pillow for artwork generation. The client can
also be built from a checkout without running either suite.
Native UI previews can be generated in the Linux build mirror with
`bash scripts/render_ui_preview.sh` and converted using
`scripts/collect_ui_preview.py`. See `design/ASSETS.md` for asset provenance.

Analyze a console log:

```bash
python3 scripts/analyze_stream_log.py debug.log --output report.json
```

Compilation and host tests do not establish console performance; console
validation of each release remains necessary. This tree descends from
[JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4)
(upstream commit `61427a214d4e632ee246816a98ee4f2374844a73`) and its public
release line [Akryst/Moonlight-ps4](https://github.com/Akryst/Moonlight-ps4).
