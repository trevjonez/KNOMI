"""Rebuild gif_probing / gif_qgling with the traced StealthBurner head.

The bed and the motion are left exactly as they are: for every frame the
original toolhead is erased and the new head is pasted so that its BOTTOM
(the nozzle) and horizontal centre land where the original's did -- these
animations are about nozzle-to-bed distance, so that is the anchor.

The originals are read straight out of git (they live on the master branch,
KNOMI_GIF/), so nothing is duplicated in the tree.

    python3 build_gifs.py          # writes gif_probing.gif / gif_qgling.gif here
"""
import io, subprocess, pathlib
import numpy as np
from PIL import Image
from scipy import ndimage
from draw_sb import draw

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
NAMES = ('gif_probing', 'gif_qgling')

def original(name):
    blob = subprocess.run(
        ['git', '-C', str(REPO), 'show', f'master:KNOMI_GIF/{name}/{name}.gif'],
        check=True, capture_output=True).stdout
    return Image.open(io.BytesIO(blob))

def split(arr):
    """-> (head mask, head bbox); the bed is the widest connected component."""
    fg = arr[:, :, 3] > 10
    lbl, n = ndimage.label(fg)
    comps = []
    for c in range(1, n + 1):
        ys, xs = np.where(lbl == c)
        comps.append((c, int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())))
    bed = max(comps, key=lambda t: t[4] - t[3])
    head = [t for t in comps if t[0] != bed[0]]
    mask = np.isin(lbl, [t[0] for t in head])
    bb = (min(t[1] for t in head), max(t[2] for t in head),
          min(t[3] for t in head), max(t[4] for t in head))
    return mask, bb

_HEADS = {}

def head_for(h):
    """SB rendered at height h (cached).  gif_qgling scales the toolhead as it
    travels back/front across the bed -- 13 distinct sizes -- so this is
    resolved per frame, not once."""
    if h not in _HEADS:
        im = draw(out_h=h)
        a = np.array(im)
        ys, xs = np.where(a[:, :, 3] > 10)
        _HEADS[h] = (im, int(ys.max()), int(xs.min()), int(xs.max()))
    return _HEADS[h]

def build(name):
    im = original(name)
    frames, durs = [], []
    for i in range(im.n_frames):
        im.seek(i)
        arr = np.array(im.convert('RGBA'))
        hm, bb = split(arr)
        head, h_bot, h_left, h_right = head_for(bb[1] - bb[0] + 1)
        canvas = arr.copy()
        canvas[ndimage.binary_dilation(hm, np.ones((3, 3)))] = 0
        cim = Image.fromarray(canvas, 'RGBA')
        cim.alpha_composite(head, (int(round((bb[2] + bb[3]) / 2 - (h_left + h_right) / 2)),
                                   int(round(bb[1] - h_bot))))
        frames.append(cim)
        durs.append(im.info.get('duration', 40))

    # fixed palette, index 0 reserved for transparency
    cols = sorted({tuple(int(v) for v in c)
                   for f in frames
                   for c in np.unique(np.array(f)[np.array(f)[:, :, 3] > 10][:, :3], axis=0)})
    pal = [0, 0, 0] + [v for c in cols for v in c]
    pal += [0] * (768 - len(pal))
    idx = {c: i + 1 for i, c in enumerate(cols)}

    pf = []
    for f in frames:
        a = np.array(f)
        p = np.zeros(a.shape[:2], np.uint8)
        op = a[:, :, 3] > 10
        p[op] = [idx[tuple(int(v) for v in c)] for c in a[:, :, :3][op]]
        pi = Image.fromarray(p, 'P'); pi.putpalette(pal)
        pf.append(pi)

    # Frames are written whole with disposal=2 (restore to background): with
    # transparency present, delta frames would ghost.
    out = HERE / f'{name}.gif'
    pf[0].save(out, save_all=True, append_images=pf[1:], duration=durs,
               loop=0, disposal=2, transparency=0, optimize=False)
    chk = Image.open(out)
    tot = sum((chk.seek(i), chk.info.get('duration', 0))[1] for i in range(chk.n_frames))
    print(f'{name}: {im.size[0]}x{im.size[1]} {im.n_frames} -> {chk.n_frames} frames, '
          f'{len(cols)} colours, {tot}ms, head sizes {sorted(_HEADS)}')

if __name__ == '__main__':
    for n in NAMES:
        build(n)
