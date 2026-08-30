"""Emit the toolhead-scene sprites: one bed, N StealthBurner heads.

This replaces gif_probing / gif_qgling.  Those were canned animations; the
scene they are replaced by is composited at runtime from real toolhead
position, so instead of frames we ship:

  * sb_bed        -- the bed, lifted straight out of the original gif_qgling
                     artwork with the toolhead erased, cropped to its bbox.
  * sb_head[]     -- the SB rendered once per depth step, smallest (far) to
                     largest (near).  Discrete sprites rather than a runtime
                     lv_img_set_zoom() so the pixel art stays crisp and the
                     draw stays a plain blit.

The projection constants come out of the artwork itself (see quad() below)
rather than being typed in twice, so if the bed art is ever redrawn the C
side follows automatically.

    python3 gen_sprites.py     # -> src/sprites/toolhead_scene.{c,h}
"""
import io, subprocess, pathlib
import numpy as np
from PIL import Image
from scipy import ndimage
from draw_sb import draw

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
DST = REPO / 'src' / 'sprites'

# Height of the head sprite at the very front of the bed. The original
# animation used 58 at its nearest point, which is also about right
# physically: 58px tall is ~31px wide, and 31px of a 160px-wide front edge
# representing 350mm works out to a ~68mm toolhead.
HEAD_H_FRONT = 58


def original_qgling():
    blob = subprocess.run(
        ['git', '-C', str(REPO), 'show', 'master:KNOMI_GIF/gif_qgling/gif_qgling.gif'],
        check=True, capture_output=True).stdout
    im = Image.open(io.BytesIO(blob))
    im.seek(0)
    return np.array(im.convert('RGBA'))


def bed_only(arr):
    """The bed is the widest connected component; everything else is the head.

    The head never overlaps the bed in this artwork (its lowest pixel is
    y=69, the bed's highest is y=72), so this crops cleanly with nothing to
    inpaint."""
    fg = arr[:, :, 3] > 10
    lbl, n = ndimage.label(fg)
    comps = []
    for c in range(1, n + 1):
        ys, xs = np.where(lbl == c)
        comps.append((c, int(xs.max() - xs.min())))
    bed = max(comps, key=lambda t: t[1])[0]
    mask = lbl == bed
    ys, xs = np.where(mask)
    out = arr.copy()
    out[~mask] = 0
    return out[ys.min():ys.max() + 1, xs.min():xs.max() + 1], mask


def quad(bedimg):
    """Corners of the bed's *top face* in bed-sprite pixel coordinates.

    The art is a trapezoid (the top face, narrowing towards the back) sitting
    on a constant-width slab (the bed's side thickness). Walk the rows: while
    the top face is being drawn the row gets wider each line; once the slab
    starts the width stops changing. The last widening row is the front edge."""
    a = bedimg[:, :, 3] > 10
    rows = []
    for y in range(a.shape[0]):
        xs = np.where(a[y])[0]
        rows.append((int(xs.min()), int(xs.max())))
    widths = [r[1] - r[0] for r in rows]
    front = 0
    for y in range(1, len(widths)):
        if widths[y] > widths[y - 1]:
            front = y
    back = 0
    return {
        'back_y': back, 'back_x0': rows[back][0], 'back_x1': rows[back][1],
        'front_y': front, 'front_x0': rows[front][0], 'front_x1': rows[front][1],
    }


def head_sprites(q):
    """One sprite per 1px step of apparent height, front size down to back.

    The shrink factor is taken from the bed quad so the head foreshortens by
    exactly as much as the bed does -- half-width at the back over half-width
    at the front."""
    hw_front = (q['front_x1'] - q['front_x0']) / 2.0
    hw_back = (q['back_x1'] - q['back_x0']) / 2.0
    h_back = int(round(HEAD_H_FRONT * hw_back / hw_front))
    out = []
    for h in range(h_back, HEAD_H_FRONT + 1):
        im = draw(out_h=h)
        a = np.array(im)
        ys, xs = np.where(a[:, :, 3] > 10)
        # crop to content so the nozzle anchor is exact
        im = im.crop((int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1))
        a = np.array(im)
        # draw_sb forces left/right symmetry, so the nozzle tip is bottom-centre.
        assert np.array_equal(a[:, :, 3], a[:, ::-1, 3]), f'head h={h} not symmetric'
        out.append(im)
    return out


def rgba_to_lv(im):
    """TRUE_COLOR_ALPHA, LV_COLOR_16_SWAP=0: RGB565 little-endian + alpha."""
    a = np.array(im).astype(np.uint16)
    r, g, b, al = a[:, :, 0], a[:, :, 1], a[:, :, 2], a[:, :, 3]
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    buf = np.empty((a.shape[0], a.shape[1], 3), dtype=np.uint8)
    buf[:, :, 0] = (c & 0xFF).astype(np.uint8)
    buf[:, :, 1] = (c >> 8).astype(np.uint8)
    buf[:, :, 2] = al.astype(np.uint8)
    return buf.tobytes()


def emit_array(name, data):
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        tail = '' if i + 12 >= len(data) else ','
        lines.append('    ' + ','.join(f'0x{b:02x}' for b in chunk) + tail)
    return (f'static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST '
            f'uint8_t {name}_map[] = {{\n' + '\n'.join(lines) + '\n};\n')


def emit_dsc(name, w, h, size):
    return (f'const lv_img_dsc_t {name} = {{\n'
            f'  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,\n'
            f'  .header.always_zero = 0,\n'
            f'  .header.reserved = 0,\n'
            f'  .header.w = {w},\n'
            f'  .header.h = {h},\n'
            f'  .data_size = {size},\n'
            f'  .data = {name}_map,\n'
            f'}};\n')


def main():
    DST.mkdir(parents=True, exist_ok=True)
    bedimg, _ = bed_only(original_qgling())
    q = quad(bedimg)
    heads = head_sprites(q)

    chunks = ['''/* GENERATED by tools/stealthburner/gen_sprites.py -- do not edit.
 *
 * Sprites for the live toolhead scene. See toolhead_scene.h for the
 * projection constants, which are derived from this same bed artwork. */
#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#include "toolhead_scene.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif
''']

    bw, bh = bedimg.shape[1], bedimg.shape[0]
    bdata = rgba_to_lv(Image.fromarray(bedimg, 'RGBA'))
    chunks.append(emit_array('sb_bed', bdata))
    chunks.append(emit_dsc('sb_bed', bw, bh, len(bdata)))

    total = len(bdata)
    for i, im in enumerate(heads):
        d = rgba_to_lv(im)
        total += len(d)
        chunks.append(emit_array(f'sb_head_{i}', d))
        chunks.append(emit_dsc(f'sb_head_{i}', im.width, im.height, len(d)))

    chunks.append('const lv_img_dsc_t * const sb_head[SB_HEAD_COUNT] = {\n'
                  + ''.join(f'    &sb_head_{i},\n' for i in range(len(heads)))
                  + '};\n')
    (DST / 'toolhead_scene.c').write_text(''.join(chunks))

    hdr = f'''/* GENERATED by tools/stealthburner/gen_sprites.py -- do not edit.
 *
 * The bed sprite is the original gif_qgling bed art with the toolhead
 * erased. Its top face is a trapezoid; these are its corners in the
 * sprite's own pixel coordinates, which is what the runtime projection
 * maps printer X/Y onto:
 *
 *      ({q['back_x0']},{q['back_y']}) ______________ ({q['back_x1']},{q['back_y']})     back  (Y max)
 *              /                \\
 *             /                  \\
 *   ({q['front_x0']},{q['front_y']}) /____________________\\ ({q['front_x1']},{q['front_y']})   front (Y min)
 *
 * Head sprites run smallest (far) to largest (near); index maps linearly
 * onto that same front..back axis, so the head foreshortens by exactly the
 * factor the bed does. */
#ifndef TOOLHEAD_SCENE_H
#define TOOLHEAD_SCENE_H

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif
#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif

#define SB_BED_W        {bw}
#define SB_BED_H        {bh}

#define SB_BED_BACK_Y   {q['back_y']}
#define SB_BED_BACK_X0  {q['back_x0']}
#define SB_BED_BACK_X1  {q['back_x1']}
#define SB_BED_FRONT_Y  {q['front_y']}
#define SB_BED_FRONT_X0 {q['front_x0']}
#define SB_BED_FRONT_X1 {q['front_x1']}

#define SB_HEAD_COUNT   {len(heads)}

extern const lv_img_dsc_t sb_bed;
extern const lv_img_dsc_t * const sb_head[SB_HEAD_COUNT];

#endif /* TOOLHEAD_SCENE_H */
'''
    (DST / 'toolhead_scene.h').write_text(hdr)

    print(f'bed   {bw}x{bh}  quad back y={q["back_y"]} x={q["back_x0"]}..{q["back_x1"]}'
          f'  front y={q["front_y"]} x={q["front_x0"]}..{q["front_x1"]}')
    print(f'heads {len(heads)}  h={heads[0].height}..{heads[-1].height}'
          f'  w={heads[0].width}..{heads[-1].width}')
    print(f'total {total} bytes ({total/1024:.1f} KB)')


if __name__ == '__main__':
    main()
