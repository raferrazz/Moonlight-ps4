# Native UI artwork and typography

- `lake-background.png`: generated with an image generation tool, then embedded
  at 1280x720 by `scripts/embed_ui_background.py`. Used for Desktop and the
  Midnight/Cinema splashes. The original generated asset is preserved here.
- `native/*.png`: actual framebuffer output of the C renderer (one
  Games-tab capture per theme), converted to PNG. These are not
  mockups.
- `native-preview.gif`: a recorded scroll through the app (Games row,
  Settings and PCs tabs, Orbis theme) rendered by the same C menu
  renderer, captured at 25 fps.
- Icons, rounded borders, glows and wave ribbons are drawn by native C code.
  The splash uses Moonlight's eight-segment circular logo, matching the package
  icon `pkg/sce_sys/icon0.png`, with the theme's text color.
- The proportional font atlas is generated from Carlito (SIL OFL 1.1)
  regular/bold by `scripts/generate_ui_font.py`; the license text is in
  `vendor/FONT-LICENSE.txt`. The source TTF can be supplied by Google Fonts
  or a Linux distribution.

## Image generation prompt (lake-background.png)

Create one production background asset for a PlayStation Moonlight streaming
client, 16:9 landscape. Photorealistic calm alpine lake at deep blue twilight/night,
silhouettes of forest and layered mountain ridges framing both sides, distant low
peaks centered on horizon at 65% of image height, subtle starlit navy sky taking
upper 60%, lake reflections in bottom 35%. Rich dark navy, muted cyan, faint warm
light at horizon. Elegant restrained cinematic atmosphere, no buildings, people,
moon, logos, text, symbols, interface or borders. Large clean dark sky space at
upper-middle for a crescent logo and white typography that will be drawn by the
application. Detailed landscape, soft natural light. It should match the lake and
mountain mood of the Midnight and Cinema UI concept splash screens.
