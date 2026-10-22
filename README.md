<div align="center">

# Moonlight

### Release 1.5.5 · Experimental

Stream your PC games to a jailbroken PS4 with Sunshine and a DualShock 4.

![Version](https://img.shields.io/badge/release-1.5.5-36CADB?style=flat-square&labelColor=172B40)
![Platform](https://img.shields.io/badge/tested-PS4%20%2F%209.00%20%2F%20GoldHEN-0B1525?style=flat-square&labelColor=172B40)
![Target](https://img.shields.io/badge/target-1080p60-36CADB?style=flat-square&labelColor=172B40)

<img src="design/console/midnight-games-ps4-01.33.jpg" alt="Moonlight PS4 01.33 Games menu — actual PS4 screenshot" width="800">

<sub>Captured on an original PS4 running firmware 9.00 with GoldHEN · Midnight theme · Client 01.33.</sub>

**[What's new](#whats-new-in-155)** · **[Install](#installation)** · **[Controls](#controls)** · **[Known issues](#known-issues)**

**[Download PKG](../../releases/download/v1.5.5/Moonlight-PS4-01.55.pkg)** · **[Release notes & checksums](../../releases/tag/v1.5.5)**

</div>

> **Unofficial client:** Moonlight PS4 is independently developed. It is not
> affiliated with, endorsed by, or maintained by the official Moonlight project.
> Product names belong to their respective owners.

## What's new in 1.5.5

Everything below is new since 01.35.

### In-stream quick menu

- **Hold L3 + R3 (~2.5 s) while streaming to open the quick menu** — a small
  panel over the stream with the things you actually change mid-game. A
  progress ring shows the hold.
  - *Touchpad mouse* — toggles instantly, saved for the next session.
  - *Performance overlay* — the live FPS/decode/latency panel.
  - *Bitrate (kbps)* — LEFT/RIGHT steps in the Settings increments
    (5,000–50,000); closing the menu with a new value reconnects the stream
    seamlessly (the game keeps running on the host).
  - *Quit stream* — back to the client menu, game left running on the PC.
  - *Quit app on PC* — ends the stream *and* closes the game on the PC (the
    same request as pressing OPTIONS over a running game in the app list),
    then returns to the Games list. Moonlight keeps running.
- **Input is safe while the menu is up.** Passthrough to the host pauses
  (each pad sends one neutral frame, so the game stops eating held buttons),
  pad 0 drives the menu, and after closing, passthrough resumes only once
  each pad reads neutral — nothing held while browsing leaks into the game.

### Controllers and input

- **Local co-op with up to 4 controllers.** Every DualShock 4 that is
  logged in on the console is forwarded as its own gamepad to the host
  (P1 blue, P2 red, P3 green, P4 pink lightbars). Pads that join
  mid-stream are picked up automatically, and a disconnected pad releases
  its held buttons so characters stop running.
- **Rumble works.** In-game rumble drives the DS4 motors (low/high
  frequency, per pad).
- **Steam's lightbar color now drives the DualShock 4.** When the host
  (Steam via Sunshine) sets a per-controller lightbar color, the physical
  DS4 mirrors it. Player colors return automatically when the stream ends.
- **Touchpad as a mouse.** Drag a finger to move the host cursor, click to
  left-click, click with two fingers to right-click, and drag two fingers
  to scroll (smooth high-resolution wheel). Games never see it as a
  touchpad while mouse mode is on; Settings → *Touchpad mouse* turns it
  off.
- **Gyro + accelerometer (motion) support.** Games on the host that
  request motion input get DS4 IMU reports automatically. Settings →
  *Gyro motion input* (default on) turns the feature off for titles where
  motion aiming misbehaves.
- **Input is polled at 250 Hz while streaming**, shaving ~4 ms of average
  button-to-host latency.

### Connection and reliability

- **Auto-reconnect on drop.** A mid-stream drop (network blip, Sunshine
  restart, PC reboot) no longer dumps you into the menu: the game is still
  running on the PC, so the client shows a "RECONNECTING" screen, tries to
  resume it three times automatically, then waits with X-to-retry /
  CIRCLE-for-menu.
- **Launches answer on screen.** An animated launch screen shows the game's
  real name, the live step ("Asking your PC to start..."), a spinner and an
  elapsed counter — and CIRCLE cancels. A handshake failure against a
  session that is still live host-side cancels the stuck session and
  relaunches in the same click. Failures stay on the screen with the PC's
  own error message, X to retry, TRIANGLE to refresh the app list first.
- **Leaving the app no longer crashes.** The PS4's *Close Application*
  kills the title in a way no catchable signal or `exit()` path can
  intercept, which used to show the CE-34878-0 dialog on roughly half of
  all closes. **Settings → Exit Moonlight** runs the sanctioned title-side
  exit (`sceSystemServiceLoadExec("exit")`), the SYS_exit override covers
  the CRT paths, and seven fault handlers convert any in-app fault into a
  clean exit as well.
- **Connect failures are named.** "Connection refused" means the PC is
  reachable but Sunshine is not listening; "connect timeout" means packets
  go unanswered.
- **The PCs tab is instant and honest.** Your saved PC is listed the moment
  the menu opens (zero network), a background `/serverinfo` probe fills in
  its hostname and an Online/Offline dot, and TRIANGLE re-checks. Subnet
  discovery sweeps the local network over plain TCP/UDP unicast — the one
  method that works on this console — instead of multicast mDNS, which the
  PS4 never receives.
- **Wake-on-LAN configures itself.** Sunshine tells paired clients the
  PC's MAC address over HTTPS; the client adopts it automatically (kept
  fresh if the network card changes) and Settings shows where the value
  came from ("(auto)"). SQUARE on the PCs tab sends the wake burst, a
  failed connect auto-sends it, and manual entry remains as a fallback.
- **The optional Windows Steam companion is gone.** Sunshine alone is the
  host: the games list comes straight from Sunshine's `/applist`, and no
  probe to a Windows listener runs (or stalls) anymore.

### Performance

- **The Games list opens instantly.** The application list and cover art
  are cached on disk and drawn immediately; the network refresh runs in
  the background and fills the grid a moment later. Art fetch and PNG
  decode run on a worker thread, fetched once ever instead of once per
  launch.
- **No request can stall the app.** On Orbis a TCP connect to a
  firewall-dropped port blocks for the kernel's full ~75-second SYN
  budget, and none of the non-blocking modes work. The HTTP layer now
  splits its locks, bounds every request with its own timeout, pools
  keep-alive connections, and runs slow work on worker threads — one dead
  host can no longer freeze the menu or the launch flow.
- **Full TLS handshakes only.** Sunshine never answers resumed TLS
  session ClientHellos on the `/launch` path, which could hang every
  launch attempt for its full 15-second budget. Every connection now pays
  one ~300 ms full handshake instead.
- **Faster menu compositing.** Flip buffers are mapped write-combined for
  the menu path, skipping the per-frame cache flush, and the menu only
  flips on change instead of redrawing at ~40 fps.
- **Debug logs keep themselves useful:** every line is timestamped
  (`[t=12.345]`) and the file log rotates once it passes 1 MB.

### Interface

- **New default theme: Orbis** — the PS4 system look (deep navy gradient,
  light-blue accent, white text). Six themes ship:
  **Orbis · Midnight · Mono · Blue Wave · Daylight · Cinema**; existing
  configurations keep their saved theme.
- **A sharpness pass over the whole renderer.** Text is baked from a 64 px
  Carlito master and area-average downscaled (crisp at every size), box
  art is bilinear with anti-aliased rounded corners, and every icon and
  button prompt is anti-aliased. The sidebar icons are solid
  Material-style vector shapes whose cutouts show the live theme
  gradient.
- **Settings scrolls.** Twice the text size (30 px labels in 96 px rows)
  with a scrollable list, an eased follow-the-cursor view, a scrollbar,
  and a one-line hint that explains the selected row (WOL state, gyro,
  overlay combo, touchpad gestures, bitrate guidance).
- **The in-stream UI matches the menu:** the performance overlay is a
  translucent panel (FPS, pipeline timings, a Network line with wire
  spread, host latency and measured bitrate, frame-time bars) and the
  start-of-stream combo hints render as a bottom pill.
- **Resume when the game is already running.** If the selected app is the
  one running on the host, the button reads **Resume**.
- **SQUARE on a game marks it as the default** — the client connects and
  launches it automatically at boot (CIRCLE cancels into the menu), with
  the game's real name on the launch screen.
- **Renamed to just "Moonlight"** — home-screen title, splash, header and
  logs drop the "PS4" suffix.

### Audio

- **Optional 5.1 surround sound** (Settings → *Surround sound (5.1)*):
  asks Sunshine for 5.1 audio and plays it through the PS4's 8-channel
  HDMI output, with an automatic stereo downmix if the TV/AVR refuses
  8-channel. Experimental: on some receivers the channel order can
  require a one-line mapping fix.

## Included features

- Local IPv4 discovery (subnet sweep + saved-host probe), manual IP entry
  and PIN pairing.
- Sunshine application list, launch/resume and Desktop streaming.
- H.264 hardware decoding, stereo (optionally 5.1) Opus audio and
  DualShock 4 input for up to four pads.
- Touchpad mouse, motion input, rumble and Steam lightbar mirroring.
- Wake-on-LAN with auto-detected MAC.
- In-stream quick menu (hold L3 + R3): toggles, bitrate, quit actions.
- Six saved themes: **Orbis (default) · Midnight · Mono · Blue Wave ·
  Daylight · Cinema**.
- Matching startup splashes, portrait covers and game panoramas.
- Balanced / Low latency modes, performance overlay and file logging.
- Auto-reconnect on drops; default app with boot auto-launch.
- Return to the client menu and a controller shortcut for the host Guide
  button.

<details>
<summary><strong>Preview the themes</strong></summary>

![Splash with the Moonlight logo — native renderer preview](design/native-01.33/midnight-splash.png)

![Themes, splash screens and settings](design/native-themes-preview-01.33.png)

</details>

<sub>Images are previews produced by the actual C UI renderer on a development
PC, not PS4 captures. The previews show the pre-1.5.5 UI; the header image above is
an actual PS4 screenshot. Game artwork belongs to its respective owners.</sub>

## Installation

**Tested setup:** original PS4, firmware **9.00**, GoldHEN, DualShock 4 and an
Ethernet-connected PC running [Sunshine](https://github.com/LizardByte/Sunshine).
The Windows test host uses ViGEmBus for controller emulation.

- **Package:** `Moonlight-PS4-01.55.pkg`
- **Title ID:** `MLNT00001`
- **Settings:** `/data/moonlight`

Download the PKG and `SHA256SUMS` from the
[1.5.5 release](../../releases/tag/v1.5.5).
Build from source using [docs/BUILD.md](docs/BUILD.md).

1. Install the PKG with GoldHEN's Package Installer. It updates an existing
   client using the same title ID and retains its saved settings.
2. Open **PCs** and press **Triangle** to discover your PC, or enter its IP in Settings.
3. Connect with **X**, then enter the displayed PIN in Sunshine's pairing page.
4. Select a game in **Games** and press **X** to play.

Start with **1080p · 60 FPS · 20,000 kbps · Hardware decoding · Balanced**.
For TruckersMP or another external launcher, choose **Desktop** and launch the
game on your PC. To stream Steam games, add them to Sunshine's application
list (Sunshine publishes Steam games on its own; no companion software is
needed).

## Controls

| Input | Action |
| --- | --- |
| D-pad | Navigate (Settings scrolls and follows the cursor) |
| X / Circle | Confirm / back |
| Triangle | Discover PCs / refresh applications |
| L1 / R1 | Switch menu tabs |
| Square on a game | Set it as the default app (auto-launches at boot) |
| Square on the PCs tab | Send a Wake-on-LAN packet (MAC auto-detected after the first connect) |
| Options in Games | Close the active host session |
| L1 + R1 + Options while streaming | Send Guide to the host |
| Hold L3 + R3 (~2.5 s) while streaming | Open the quick menu (progress ring shows the hold) |
| Quick menu | Toggle touchpad mouse / perf overlay, change bitrate (applies via a quick reconnect), quit stream (game keeps running on the PC), quit app on PC (also closes the game on the PC) |
| Touchpad drag / click / two-finger click while streaming | Move / left click / right click the host mouse |
| Two-finger drag on the touchpad while streaming | Scroll (high-resolution mouse wheel) |
| Steam lightbar color | Applied to the DualShock 4 lightbar while streaming (per player slot) |

The physical PS button opens the PS4 system menu. Use the Guide shortcut for Steam.

## Known issues

- **Locked 60 FPS is not guaranteed.** An earlier ETS2 Desktop-stream sample
  averaged 58.5 client-reported FPS over about 8.5 minutes, with the final
  minutes near 59.9 FPS. Sunshine captured at 59.94 Hz.
- A 01.33 Stellar Blade sample averaged 59.0 client-reported FPS over 217 seconds,
  with 33 local buffer-starvation drops. This is the baseline for testing the 01.34 presentation policy,
  not a measurement of the new release's performance.
- The presentation changes introduced in 01.34 need console comparison.
  The apparent sharpening of the picture has not been conclusively
  diagnosed.
- Compatibility beyond the original PS4 / firmware 9.00 setup is unverified.
- Discovery currently covers local IPv4; use manual IP if multicast is unavailable.
- USB keyboard/mouse support and native YCbCr presentation remain unfinished.
- Steam games appear through Sunshine's application list; the client does not
  scan Steam directly.

Detailed measurements and the next test procedure:
[Stellar Blade presentation test](docs/PRESENTATION_01_34.md) and
[ETS2 performance analysis](docs/ETS2_01_32.md).
Experimental YCbCr plugins are outside the standard installation.

## Credits

Based on [JaimeJimenezG/Moonlight-ps4](https://github.com/JaimeJimenezG/Moonlight-ps4),
using [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c),
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), Sunshine,
FFmpeg, Opus and mbedTLS. Dependency pins: [third_party/DEPS](third_party/DEPS).
Font notices: [vendor/FONT-LICENSE.txt](vendor/FONT-LICENSE.txt).
UI asset provenance: [design/ASSETS_01_31.md](design/ASSETS_01_31.md).

A consolidated top-level license notice remains pending. Existing dependency
licenses and notices apply.
