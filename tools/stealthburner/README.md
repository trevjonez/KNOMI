# StealthBurner busy-state icons

Redraws the `PROBING` and `QGLING` animations so the toolhead reads as a
Voron StealthBurner instead of the stock BTT nozzle icon.

Only the **head** is replaced. The bed art, the frame count, the per-frame
delays and the motion are the originals', so the animations play exactly as
before.

## Geometry

`draw_sb.py` is not free-hand art — the silhouette and element ratios were
extracted programmatically from a StealthBurner reference render, then
cleaned up:

* the outline is an hourglass (wide shoulders, waist, widest near the
  bottom), traced from the reference's own edge profile;
* two materials, as on the real part: the light-grey body housing with the
  face plate inset on top;
* the face plate notches up around the Dragon heat block, which is drawn
  with straight sides, a flat bottom and a tapered nozzle (the reference
  render wears a silicone sock, which rounds that profile off);
* shapes are stored as **left-half chains** and mirrored about `AXIS` at
  draw time, and the raster is force-symmetrised before the (deliberately
  diagonal) Voron mark is applied, so the part is symmetric to the pixel;
* the intake hexagon is sized so its gap to the Voron mark equals the mark's
  gap to the fan — change `logo_scale` and it stays matched.

Tunables on `draw()`: `logo_scale`, `hex_round`, `face_margin`, `smooth`.

## Rebuilding

    python3 build_gifs.py     # reads the originals from master:KNOMI_GIF/
    python3 gen_c.py          # writes ../../src/gif/gif_{probing,qgling}.c

Needs `pillow`, `numpy`, `scipy`.

`gen_c.py` emits the same shape the other generated files use: the raw `.gif`
bytes in an `lv_img_dsc_t` marked `LV_IMG_CF_RAW_CHROMA_KEYED`, which LVGL's
own GIF decoder (`LV_USE_GIF`) plays back via the `lv_gif` widget. No symbol
names change, so `lv_overlay.h` and `lv_moonraker_change_screen.cpp` are
untouched.

Frames are written whole with `disposal=2`; delta frames would ghost against
the transparent background.
