# Performance Notes — Orbis UI overhaul

Measured/derived on the host render harness (same software rasterizer, same
draw paths as the console) plus code-path analysis. Console frame-timing
validation remains on the console checklist.

## What got cheaper

1. **Background painting: per-redraw → cached.** The old `ui_theme_background()`
   re-rendered the full-screen gradient (1 blend per scanline) plus, on Blue
   Wave/Orbis, two per-column ribbon loops with up to ~60 layered pixel writes
   per column — on EVERY redraw. The background is now rendered once per theme
   into a 1920×1080 BGRA cache and every later redraw is a row-pointer memcpy
   loop (~8.3 MB). Estimated win on Jaguar-class cores: tens of ms of CPU per
   redraw removed during animation windows; identical visuals.

2. **Present-on-change preserved, and animation windows are bounded.** The
   menu loop still flips only when something changes. New animations (hero-row
   glide, tab slide, toast, quick-menu slide) keep the loop dirty for at most:
   - hero glide: ~200 ms settle (time-based exponential ease, tau 36 ms)
   - tab slide: 190 ms, toast ≤ 4.5 s total (single panel), quick menu 300 ms
   After settling, the loop returns to zero-flip idle. No animation runs
   unbounded; none redraws without presenting.

3. **Draw-call budget per menu frame (Games tab, 10 tiles):** one 8.3 MB
   background memcpy + ≤7 tile blits (bilinear; during glides each (app, size)
   comes scaled-once from the render-cache ladder) + ≤2 focus composites +
   ~30 AA text labels. The bg cache makes the full-frame hero composition
   net-cheaper than the pre-overhaul four-tile Orbis layout at equal frame
   rates.

4. **Stream HUD:** the quick menu draws a fixed-size left panel + one content
   card per frame (~0.55 Mpx of alpha blending) versus the old centered panel
   of similar area — same cost class, still drawn only while open (or during
   its 300 ms reveal). Perf overlay and hold ring unchanged in cost.

## What to watch on console

- The hero-row glide redraws the full frame for ~200 ms per step. With the bg
  cache and the render-cache ladder this is a memcpy plus cached blits, and the
  loop paces at 1 ms while anything animates, so the present cadence sets the
  frame rate.
- The toast is drawn on top of the menu (never over the stream).
- Everything else in the streaming path (decoder, presentation policy, input
  polling, network threads) is untouched by this overhaul.

## Harness numbers (host, -O1, ASAN on — relative, not console absolute)

- Full 15-scenario harness: all pass, 69 captured states; wall time ~75 s
  under ASAN (dominated by real-time scenario sleeps, not rendering).
- Unit suites: 163/163 assertions, < 1 s total.
- UBSAN clean across all rendering paths exercised (area-average text, bilinear
  blits, AA shapes, cached backgrounds, all six themes).

## Icon blits

The keycap chips and pictograms blit through the text path's span sampler
(box-filter downscale, ≤4 source taps per axis) with two passes per chip (body
+ ink). A full function row is 5-7 chips ≈ 5-7 k blended pixels — well under one
30 px text label each; the OSK grid (24 keys) adds ~60 k mask-sampled pixels per
frame, but the OSK only draws while open. The loading spinner is throttled to
~25 redraws/s (not per loop pass) so the present-on-change discipline holds.
Chip masks are 98 KB of .rodata (12 chips at 64x64x2) + 47 KB pictograms; the
font master gained 2 glyphs (middle dot) for 582 KB total.
