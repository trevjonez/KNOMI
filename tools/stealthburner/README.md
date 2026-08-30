# StealthBurner toolhead scene

Replaces the `PROBING` and `QGLING` animations with a scene composited at
runtime from the printer's **real toolhead position**, drawn as a Voron
StealthBurner instead of the stock BTT nozzle icon.

The originals were canned loops: a head bouncing along a fixed path that had
nothing to do with what the machine was doing. This ships sprites instead of
frames, and the firmware places them — see
`src/ui_overlay/lv_toolhead_scene.cpp`.

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

## The projection

The bed is the original `gif_qgling` artwork with the toolhead erased. Its
top face is a trapezoid, and that trapezoid *is* the projection: printer X/Y
maps onto it, and the head foreshortens by exactly the factor the bed does
(half-width at the back over half-width at the front).

`gen_sprites.py` measures the trapezoid off the artwork rather than having
it typed in, and writes the corners into `toolhead_scene.h` beside the
sprites, so the C side follows automatically if the bed art is ever redrawn.
The head is rendered once per 1px step of apparent height — discrete sprites
rather than a runtime `lv_img_set_zoom()`, so the pixel art stays crisp and
the draw stays a plain blit.

## Rebuilding

    python3 gen_sprites.py     # -> ../../src/sprites/toolhead_scene.{c,h}
    python3 preview_scene.py   # -> preview_scene.png, off-device check

Needs `pillow`, `numpy`, `scipy`. Both run in the build container:
`./tools/docker/run.sh assets`.

`preview_scene.py` mirrors `scene_apply()` in the firmware so the geometry
can be checked without a flash cycle — it renders the bed corners and a Z
sweep, and reports how close each lands to the round display's 120px radius.
It is a preview, not the source of truth; if the C maths changes, change it
too.

## History

Before this, the same art was composited into replacement **GIFs** — the
head swapped into every original frame, bed and timing untouched. Two
things that cost real time there and are worth not rediscovering:
`gif_qgling` scales the toolhead across 13 sizes as it travels back/front,
so the head had to be re-rendered per frame rather than pasted at one size;
and frames had to be written whole with `disposal=2`, because against a
transparent background delta frames ghost the previous head. Both are moot
now that placement is done at runtime, but `build_gifs.py` and `gen_c.py`
are in git history if that path is ever wanted back.
