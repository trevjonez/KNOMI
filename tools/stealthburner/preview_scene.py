"""Render the toolhead scene off-device, to check the projection.

This mirrors scene_apply() in src/ui_overlay/lv_toolhead_scene.cpp -- same
constants, same maths -- so the geometry can be checked without a flash
cycle. It is a preview, not the source of truth; if the C changes, change
this with it.

Two things it answers:
  * does the composition stay inside the round display's 240px circle at the
    extremes (bed corners, high Z), and
  * does it read correctly -- head over the right part of the bed, shadow
    under it, size falling off towards the back.

    python3 preview_scene.py     # -> preview_scene.png
"""
import math, pathlib, re
import numpy as np
from PIL import Image, ImageDraw
from draw_sb import draw
import gen_sprites

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
SPRITES = REPO / 'src' / 'sprites'

SCREEN = 240
SCENE_BED_Y = 138
LABEL_GAP = 8
HOME_Z_MM = 15.0
Z_NEAR_PX_PER_MM = 1.35
Z_MAX_PX = 45.0

# printer extents, as reported by the machine this was built for
AXIS_MIN = (-5.0, 0.0)
AXIS_MAX = (351.0, 356.0)


def constants():
    """Read the generated header so this cannot drift from the sprites."""
    txt = (SPRITES / 'toolhead_scene.h').read_text()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r'#define\s+(SB_\w+)\s+(\d+)', txt)}


def bed_image():
    # same extraction the sprite generator uses, so the preview cannot be
    # showing a different bed than the firmware ships
    arr, _ = gen_sprites.bed_only(gen_sprites.original_qgling())
    return Image.fromarray(arr, 'RGBA')


C = constants()
BED = bed_image()
BED_X = (SCREEN - C['SB_BED_W']) // 2
HEADS = None


def heads():
    global HEADS
    if HEADS is None:
        hw_f = (C['SB_BED_FRONT_X1'] - C['SB_BED_FRONT_X0']) / 2
        hw_b = (C['SB_BED_BACK_X1'] - C['SB_BED_BACK_X0']) / 2
        h_back = int(round(58 * hw_b / hw_f))
        HEADS = []
        for h in range(h_back, 58 + 1):
            im = draw(out_h=h)
            a = np.array(im)
            ys, xs = np.where(a[:, :, 3] > 10)
            HEADS.append(im.crop((int(xs.min()), int(ys.min()),
                                  int(xs.max()) + 1, int(ys.max()) + 1)))
    return HEADS


def place(u, v, z):
    """-> (head_xy, head_size, contact_xy, shadow_size), matching scene_apply()."""
    front_cx = (C['SB_BED_FRONT_X0'] + C['SB_BED_FRONT_X1']) / 2
    back_cx = (C['SB_BED_BACK_X0'] + C['SB_BED_BACK_X1']) / 2
    front_hw = (C['SB_BED_FRONT_X1'] - C['SB_BED_FRONT_X0']) / 2
    back_hw = (C['SB_BED_BACK_X1'] - C['SB_BED_BACK_X0']) / 2

    cx = front_cx + (back_cx - front_cx) * v
    hw = front_hw + (back_hw - front_hw) * v
    row_y = C['SB_BED_FRONT_Y'] + (C['SB_BED_BACK_Y'] - C['SB_BED_FRONT_Y']) * v

    contact = (round(BED_X + cx + (u - 0.5) * 2 * hw), round(SCENE_BED_Y + row_y))
    depth = hw / front_hw
    idx = max(0, min(C['SB_HEAD_COUNT'] - 1, round((1 - v) * (C['SB_HEAD_COUNT'] - 1))))
    sp = heads()[idx]
    lift = Z_MAX_PX * (1 - math.exp(-(z * Z_NEAR_PX_PER_MM) / Z_MAX_PX)) * depth
    head_xy = (contact[0] - sp.width // 2, contact[1] - round(lift) - sp.height)
    sw = round(sp.width * 0.85)
    return head_xy, sp, contact, (sw, max(3, sw // 3))


def frame(u, v, z, label, state=None):
    im = Image.new('RGBA', (SCREEN, SCREEN), (0, 0, 0, 255))
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, SCREEN - 1, SCREEN - 1], outline=(40, 40, 40, 255))
    head_xy, sp, contact, (sw, sh) = place(u, v, z)

    # the shadow is a child of the bed image in LVGL, so it is clipped to it;
    # model that by drawing it into a bed-sized layer masked by the bed's alpha
    bed = BED.copy()
    lay = Image.new('RGBA', bed.size, (0, 0, 0, 0))
    bx, by = contact[0] - BED_X, contact[1] - SCENE_BED_Y
    ImageDraw.Draw(lay).ellipse([bx - sw // 2, by - sh // 2,
                                 bx + sw // 2, by + sh // 2], fill=(0, 0, 0, 150))
    lay.putalpha(Image.fromarray(
        (np.array(lay)[:, :, 3] * (np.array(bed)[:, :, 3] > 10)).astype(np.uint8)))
    bed.alpha_composite(lay)

    im.alpha_composite(bed, (BED_X, SCENE_BED_Y))
    im.alpha_composite(sp, head_xy)

    if state:
        # the firmware uses montserrat_16; PIL's default is smaller, so this
        # checks placement rather than exact metrics. The box is the real
        # 16px-tall extent the label will occupy.
        top = SCENE_BED_Y + C['SB_BED_H'] + LABEL_GAP
        d.text((SCREEN // 2, top + 8), state, fill=(255, 255, 255, 255),
               anchor='mm')
        d.rectangle([SCREEN // 2 - 34, top, SCREEN // 2 + 34, top + 16],
                    outline=(60, 60, 60, 255))

    d.text((4, 4), label, fill=(200, 200, 200, 255))
    return im, head_xy, sp


def main():
    cases = []
    for name, (X, Y) in [('front-left', (0, 0)), ('front-right', (350, 0)),
                         ('back-left', (0, 350)), ('back-right', (350, 350)),
                         ('centre', (175, 175))]:
        for z in (0, 10, 50):
            u = (X - AXIS_MIN[0]) / (AXIS_MAX[0] - AXIS_MIN[0])
            v = (Y - AXIS_MIN[1]) / (AXIS_MAX[1] - AXIS_MIN[1])
            cases.append((u, v, z, f'{name} Z{z}', 'Probing'))

    # the homing sweep, as homing_target() computes it: X sweeps first from
    # centre, then Y, then Z descends at bed centre
    for lbl, (u, v, z) in [
            ('home X start',  (0.50, 0.50, HOME_Z_MM)),
            ('home X mid',    (0.75, 0.50, HOME_Z_MM)),
            ('home X done',   (1.00, 0.50, HOME_Z_MM)),
            ('home Y mid',    (1.00, 0.75, HOME_Z_MM)),
            ('home Y done',   (1.00, 1.00, HOME_Z_MM)),
            ('home Z centre', (0.50, 0.50, HOME_Z_MM)),
            ('home Z touch',  (0.50, 0.50, 0.0))]:
        cases.append((u, v, z, lbl, 'Homing'))

    tiles, worst = [], 0.0
    for u, v, z, label, state in cases:
        im, (hx, hy), sp = frame(u, v, z, label, state)
        # furthest opaque pixel from the display centre
        a = np.array(sp)
        ys, xs = np.where(a[:, :, 3] > 10)
        r = np.hypot((xs + hx) - SCREEN / 2, (ys + hy) - SCREEN / 2).max()
        worst = max(worst, r)
        flag = '  <-- OUTSIDE BEZEL' if r > 120 else ''
        print(f'{label:<18} head=({hx:3d},{hy:3d}) {sp.width:2d}x{sp.height:2d} '
              f'max_r={r:5.1f}{flag}')
        tiles.append(im)

    cols = 3
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new('RGBA', (cols * SCREEN, rows * SCREEN), (0, 0, 0, 255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, ((i % cols) * SCREEN, (i // cols) * SCREEN))
    sheet.save(HERE / 'preview_scene.png')
    print(f'\nworst radius {worst:.1f} of 120 available')
    print(f'wrote {HERE / "preview_scene.png"}')


if __name__ == '__main__':
    main()
