# Orbis Design System (Moonlight PS4 UI)

Derived from a PS4 Orbis OS reference pack (15 screenshots: home/dynamic menu, Settings,
power sub-menus, Library, notifications, storage panel, in-game quick menu) and the written
Orbis design language. No Sony assets, fonts, icons or trademarks are used or bundled; only
the design language is recreated. Motion timings are set from the PS4's characteristic feel
(snappy ease-out, ~150–350 ms).

## Layout grid (1920×1080)

| Zone | Geometry | Content |
|------|----------|---------|
| Header | y 40–178 | App wordmark (tracked caps, 34 px) left; connection/stream chips right; tab strip (30 px labels + accent underline) at y≈118; hairline separator at y=178 |
| Content | y 200–920 | One native layout per tab (below) |
| Function row | y 1004–1050 | Real PS keycap chip (30 px tall, asset) + 22 px label, groups left-anchored; L1/R1 keycap chips + "Tabs" right-aligned |
| Toasts | x right, y 200 | Notification panels (icon + 24 px text), slide in from right |
| Dialogs | centered | 2 px white frame panel over scrim: title 40, body 28, function hints inside bottom |

## Games tab — PS4 dynamic-menu pattern

- Horizontal row of square tiles, all **top-aligned at y=300**; the focused tile is the
  **hero**: enlarged to 384 px, left-anchored at x=64, with the primary action bar flush
  inside its bottom edge — the reference "Iniciar" treatment: full-width dark translucent
  overlay (no glyph), centered white verb, the white focus ring wrapping tile + bar.
- Neighbors at 176 px (≈46% of the hero, per the reference capture), packed at a constant
  24 px gap. Slot positions are cumulative from the glide pair (midpoint on a linear
  path), so no two tiles can ever overlap at any point of the glide.
- Focused game title in large type (58 px, regular weight) below the small-tile band
  (y=520), right of the hero + a small meta line `3 of 40 · 1080p / 60 FPS target ·
  AUTO-START` and a green-dot "Running on your PC" line; the block cross-fades while
  the row glides.
- Row position eases per selection step (~200 ms ease-out); tile growth interpolates
  through the slide (one animated variable: the fractional selection).

## PCs / Settings tabs — PS4 list pattern

- List rows: 96 px pitch, label 30 px left / value 28 px right, hairline separators.
- **Focus = a 2 px white frame wrapping the whole row, sharp corners, no fill change**
  (the exact pattern in the reference Settings and power menus).
- Selected Settings row shows its one-line hint *inside* the focused frame (the reference
  power-menu description pattern); scroll eases to follow the selection.

## Focus & motion

- Tiles: white ring + soft glow, scale eased over ~200 ms (60 fps-feel: settle in ≤12
  frames at the menu cadence).
- Rows: frame fade-in ≤150 ms; no layout shift on focus.
- Tabs: content slides 36 px with fade, ~180 ms.
- Toasts: slide-in 250 ms, hold 4.2 s, fade-out 250 ms.
- Quick menu (stream): slides from the left edge, ~300 ms, translucent navy over the game.
- All windows bounded; the menu keeps its present-on-change redraw policy (idle = zero
  flips).

## Color (Orbis theme, retuned to the stock PS4 blue)

| Token | Value | Notes |
|-------|-------|-------|
| Background | #10305F → #041838 vertical blend | Stock-like PS4 blue with two soft diagonal ribbons + top-right sheen |
| Panel | #0D2450 (opaque) / #0A1830 @ ~215 (overlay) | Dialogs, OSK, quick menu |
| Text | #FFFFFF | Primary |
| Secondary | #A9BEDC | Meta, values, hints |
| Accent | #4C9DF5 | Values on focus, toggles, live text |
| Action fill | #1F6FC4 | Hero action bar ("Play/Resume") |
| Warn | #FFC46A | Offline dots, warnings |
| Frame | #FFFFFF (dark themes) / #23415F (Daylight) | 2 px focus frames |

Other themes keep their palettes and background treatments (gradient, Blue Wave ribbons,
Orbis arcs, Daylight light scheme) and flow through the same native layout.

## Typography (Carlito, OFL; PS4's SST feel at UI sizes)

Sentence case everywhere except tracked all-caps micro-labels (verb lines, "MOONLIGHT").
Scale: hero title 54 · section header 34–36 · row label 30 · value 28 · function row 26 ·
hint/meta 24 · footer small 22. No text overflows: every string passes label-fit with
ellipsis (harness-verified).

## Stream HUD (native system layer)

- **Quick menu**: left-edge panel (~560 px, full height) — the reference "Menu rápido"
  layout: header + hairline, icon+label rows, focused row in a 2 px accent frame, and a
  right-hand content card with the selected row's control (PS4 checkbox for toggles,
  minus/plus stepper for bitrate), footer verbs. Translucent deep navy over the stream.
- Perf overlay: same panel language, top-left, big fps figure, right-aligned values,
  frame-time bars.
- Hold ring: centered bottom progress ring (unchanged semantics, retuned colors).
- Combo hint: bottom-center pill (sentence case, inline glyphs), timed fade.

## Accessibility / controller

Every action reachable on a DualShock 4 alone; focus never lost (wrap-around grids and
lists, no dead ends); notices never block navigation.

## Icon assets

All button affordances render the shipped PlayStation keycap set — dark rounded keycap
with the white glyph, one icon per button (cross, circle, triangle, square, OPTIONS,
L1/R1/L2/R2/L3/R3, touchpad) — embedded by `scripts/embed_ui_icons.py` as two coverage
masks per chip (body + ink) at 64 px, plus tintable stroke masks for the monitor and
gamepad pictograms. `ui_ps_btn()` blits them with the same area-average sampler as the
text (true box filter downscaling), so chips stay crisp from 26 px (hold ring) to 46 px
(dialog verbs); disabled hints fade the whole chip via an alpha multiplier. The keycap
treatment follows the reference pack's function row; chips keep the asset's own colors
on every theme (the PS4 system chips are theme-invariant).

Loading states share one **working spinner**: eight dots on a fixed orbit with a fading
trail (`ui_spinner()`), the system "please wait" indicator, driven by wall time in the
menu and by the frame counter on the launch/connect screens (one turn ≈ 0.5–0.64 s).

### Icon set refinements

- **Uniform lining figures**: Carlito's 6 and 8 overshoot the tabular figure
  height (the pairing PIN's 8 visibly towered at 104 px). The font bake now
  rescales any digit taller than the '0' figure box, baseline-aligned and
  centered in its advance — digits read as one even row everywhere (PIN,
  counters, bitrate, timers).
- **Gauge icon** (`ui_icon` type 2): the Performance overlay row shows a dial
  with a needle (270° band + hub), the "live telemetry" pictogram; the old
  four-tick ring read as an abstract flower at 40 px.
- **PS4 checkbox** (`ui_check`): rounded-square outline plus an anti-aliased
  check mark when on (Sound/Devices pattern); the old solid-fill interior
  read as a mystery "chip" pictogram next to On/Off values.
- **Quick-menu row icons**: Touchpad mouse = gamepad asset in a thin white
  2px frame (keycap contrast), Quit stream = cross chip, Quit app on PC =
  monitor asset; Bitrate keeps the inline signal bars.
- **Hero geometry**: hero 448 px, neighbors 208 px, row top 312, title block
  at y556/60 px — tuned so the bigger tiles still pack at the constant 24 px
  gap and the title block clears the small-tile band at every glide point.
- **Perf overlay**: translucent panel (≈62% opacity), 60 fps budget guide line
  in the frame-time graph, baseline-aligned header metadata, hidden while
  the Stream menu is open (they share the top-left corner).

### Optical alignment refinements

- **Optical alignment metrics** (`ui_label_core_c`): ui_label()'s y is the
  top of a 1.375×size row box, but the ink the eye reads as "the text" —
  the core from cap top to baseline — spans master rows 20–60 of the 88-row
  bake, so its center sits 5/8×size below y. Every icon+text pair now
  centers on that line: function-row chips and their labels (was 4 px off),
  quick-menu row icons (each of the five sat at a different height; now all
  share the row midline with the label), the L1/R1 footer group (5 px below
  the CROSS chips), hint-pill chips vs copy, OSK footer, launch/connect/
  error prompts (up to 7 px), settings values (baseline-aligned to their
  labels), PCs name/status (row midline).
- **Status dots on the x-height center** (`45×size/64`): the Connected
  bullet and "Running on your PC" dot ride the lowercase body's midline,
  like console status indicators against mixed-case labels.
- **`ui_glyph_ink_span`**: per-glyph ink extents (master rows) let the OSK
  keycaps center every glyph on its own ink box — a '.' lands at the key's
  optical center exactly like a '0'.
- **Reference icon set**: toast = info badge (knocked-out "i",
  theme-tinted); On/Off = real two-state checkbox masks (states share box
  geometry); quick-menu rows = touchpad keycap / gauge / close cross;
  hold-ring center = settings gear; monitor pictogram renders white in
  dark themes (tint follows theme luminance).
- **Error-dialog scrim deepened to 170/255** — the console's dialogs leave
  the theme background barely legible; a lighter dim let the background
  ribbons read as stray artifacts. Launch/connect elapsed counters stay
  quiet during the first second ("0 s" next to a spinner read as an error
  code).

## Toast and status-copy refinements

Full-capture review plus pixel measurement of every claim. Two tokens were
refined; everything else was verified against this document's reference-derived
values:

- **Toast/notification panels: alpha 225/255 (~88%) and a near-invisible
  edge** (hairline = focus/panel blend at 25/255). The previous 250-alpha
  panel with its brighter hairline read as an opaque "web alert"; 225 lets
  the theme ghost through (verified by measurement: interior =
  225·panel + 30·bg exactly). No blur/shadow: the reference pack carries no
  banner example, and the software rasterizer has no cheap blur.
- **Status copy is white, accent stays for values/motion**: system-screen
  headlines (ui_show_status line1: "Could not connect", "Paired OK"...),
  launch phase text, connect-screen status, and fail-dialog reason text
  render TEXT/DIM instead of accent blue — saturated blue status copy reads
  as non-native; the spinner keeps the accent as the single color pop. Tokens
  above ("Accent ... live text") narrowed to *interactive values and the
  spinner*.
- **Verified unchanged (measured)**: 2 px white sharp-corner row frames,
  hero ring (4 px graded core + 8 px accent glow), 3 px tab underline,
  toast radius 10 + slide/fade timing, footer chip ink-midline deltas
  0.69-0.75 px, Add-row plus glyph centered in its 34 px chip.
- Surfaces beyond the core shell follow the same tokens: PCs tab renders the
  [pcs] list with per-PC Online/Offline dots on the x-height midline, the
  in-place Add row (rounded plus keycap chip), OPTIONS → Forget opens the
  standard centered dialog (scrim 170, 2 px frame, X/O chips); the stream
  HUD adds the severity-colored Dropped row, fps-relative budget/verdict,
  and the one-time amber congestion pill (info-badge geometry, warn tint).

## Motion refinements and the render-cache ladder

Reference (a real-PS4 dashboard capture): the row glides one item per press
with a fast ease-out (~100-130 ms settle), tiles stay pixel-identical while
moving (pure translation, reflections in sync), only the row moves, and the
selection ring rides the focused tile. The Moonlight row already matched the
timing profile; it now also matches the raster model:

- Glide-time tile sizes snap to the render-cache ladder (TILE_W..HERO_W,
  32 px steps, floor). The growth still reads continuous at glide speed
  (the reference does not resize tiles during motion at all), and each
  (app, size) is scaled once, blitted every frame after. The settled row
  is pixel-exact: the endpoints ARE the small-tile and hero sizes.
- The packed-row invariant is unchanged: geometry spaces tiles by their
  continuous widths, drawn extents are floor-snapped (never wider than
  the slot), so overlap stays impossible at every glide point -
  script-verified across f sweeps and row lengths.
- Loop pacing: 1 ms while any animation source is active, 8 ms when idle
  (present-on-change discipline preserved for static screens).

Streaming-path tokens are untouched; this pass touched raster cost and
pacing only.

## Glide tempo

Console testing read the ~127 ms glide as slightly quick once the 60 fps
rework landed, so tau is 36 ms (settle = tau x ln(1/0.004) ~ 200 ms end to
end). The ease family, the distance clamp and the ladder snap are untouched
- only the tempo changed, and settled rows remain pixel-exact.
