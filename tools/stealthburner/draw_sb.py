"""StealthBurner icon, traced from the Taichi reference render.

Coordinates are in the reference image's own pixel space, extracted
programmatically. Shapes are stored as LEFT-HALF chains (top-centre ->
bottom-centre) and mirrored about AXIS at draw time, so the part is exactly
symmetric. Two materials as on the real part: the light-grey BODY housing
with the GREEN FACE PLATE on top; the face plate notches up around the
Dragon heat block at the bottom.
"""
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

OUTLINE = (72, 18, 18, 255)
BODY    = (139, 34, 35, 255)
FACE    = (192, 47, 48, 255)
RECESS  = (88, 22, 22, 255)

AXIS = 476.0
REF_X0, REF_X1, REF_Y0, REF_Y1 = 280, 672, 158, 882
REF_W = REF_X1 - REF_X0          # exact: 476 maps to design 50.0

BODY_L = [(476,158),(428,158),(387,172),(304,238),(288,270),(286,361),
          (318,550),(304,706),(280,797),(300,854),(376,872),(406,872),
          (426,858),(428,866),(459,866),(470,882),(476,882)]
FACE_L = [(476,174),(403,180),(328,244),(318,275),(320,568),(330,614),
          (322,702),(298,752),(318,845),(358,868),(402,872),(417,866),
          (434,815),(450,800),(476,794)]
# upper intake: straight-sided flat-top hexagon. Centre is kept where the
# traced one was; the size is derived so the hex->logo gap matches the
# logo->fan gap (see _hex_chain).
HEX_C  = (476.0, 325.0)
HEX_W0, HEX_H0 = 96.0, 87.0
BLOCK_L= [(476,786),(428,786),(428,866),(459,866),(470,882),(476,882)]

FAN_C, FAN_R = (AXIS, 646), 103
LOGO = [[(459,460),(477,460),(459,492),(441,492)],
        [(484,460),(502,460),(467,520),(451,520)],
        [(495,487),(511,487),(493,520),(476,520)]]

DESIGN_W = 100.0
DESIGN_H = (REF_Y1 - REF_Y0) * DESIGN_W / REF_W

def _chaikin(pts, iters=1):
    for _ in range(iters):
        out = [pts[0]]
        for (x0,y0),(x1,y1) in zip(pts, pts[1:]):
            out.append((0.75*x0+0.25*x1, 0.75*y0+0.25*y1))
            out.append((0.25*x0+0.75*x1, 0.25*y0+0.75*y1))
        out.append(pts[-1])
        pts = out
    return pts

def _smooth_top(chain, y_max, iters):
    if iters <= 0: return chain
    head = [p for p in chain if p[1] <= y_max]
    tail = [p for p in chain if p[1] >  y_max]
    if len(head) < 3: return chain
    return _chaikin(head, iters) + tail

def _full(chain):
    """left-half chain -> closed symmetric polygon"""
    mir = [(2*AXIS - x, y) for x, y in reversed(chain)]
    return chain + mir

def _sc(pts, s):
    return [((x - REF_X0) * s, (y - REF_Y0) * s) for x, y in pts]

def _mask(size, s, polys=(), circles=()):
    im = Image.new('L', size, 0); d = ImageDraw.Draw(im)
    for p in polys:
        d.polygon(_sc(p, s), fill=255)
    for (cx, cy), r in circles:
        (x0,y0),(x1,y1) = _sc([(cx-r, cy-r), (cx+r, cy+r)], s)
        d.ellipse([x0, y0, x1, y1], fill=255)
    return np.array(im) > 127

def _edge(a):
    return a & ~ndimage.binary_erosion(a, np.ones((3,3)))

def _scale_logo(k):
    allp = [p for bar in LOGO for p in bar]
    cx = sum(p[0] for p in allp)/len(allp); cy = sum(p[1] for p in allp)/len(allp)
    return [[(cx+(x-cx)*k, cy+(y-cy)*k) for x, y in bar] for bar in LOGO]

def _dp(pts, tol):
    """Douglas-Peucker on a polyline of (x,y)."""
    if len(pts) < 3: return list(pts)
    a = np.array(pts, float)
    st, en = a[0], a[-1]
    d = en - st; L = float(np.hypot(*d))
    if L == 0:
        dist = np.hypot(*((a - st).T))
    else:
        v = st - a
        dist = np.abs(d[0]*v[:,1] - d[1]*v[:,0]) / L
    i = int(np.argmax(dist))
    if dist[i] > tol:
        return _dp(a[:i+1], tol)[:-1] + _dp(a[i:], tol)
    return [tuple(st), tuple(en)]

def _outer(chain):
    """the part of a half-chain that descends monotonically in y"""
    ys = [p[1] for p in chain]
    i = int(np.argmax(ys))
    return chain[:i+1], chain[i+1:]

def _inset_face(face_chain, body_chain, margin, tol=1.5):
    """keep the face plate at least `margin` inside the body outline.

    The traced part lets the plate come within ~2px of the housing edge at
    the waist (y~550, level with the top of the fan); at icon scale that
    reads as the two edges touching."""
    if margin <= 0: return face_chain
    outer, rest = _outer(face_chain)
    fy = np.array([p[1] for p in outer], float); fx = np.array([p[0] for p in outer], float)
    bo, _ = _outer(body_chain)
    by = np.array([p[1] for p in bo], float); bx = np.array([p[0] for p in bo], float)
    k = np.concatenate([[True], np.diff(by) > 0]); by, bx = by[k], bx[k]
    yy = np.arange(fy[0], fy[-1] + 1e-6, 1.0)
    xx = np.maximum(np.interp(yy, fy, fx), np.interp(yy, by, bx) + margin)
    return _dp(list(zip(xx, yy)), tol) + rest

def _round_corners(chain, radius, samples=5):
    """round the interior vertices of a half-chain (endpoints sit on the axis
    and are edge midpoints, not corners, so they are left alone)."""
    if radius <= 0 or len(chain) < 3:
        return chain
    out = [chain[0]]
    for i in range(1, len(chain)-1):
        P = np.array(chain[i-1], float); V = np.array(chain[i], float)
        N = np.array(chain[i+1], float)
        d1, d2 = P-V, N-V
        l1, l2 = np.hypot(*d1), np.hypot(*d2)
        A = V + d1*(min(radius, l1/2)/l1)
        B = V + d2*(min(radius, l2/2)/l2)
        for k in range(samples+1):
            u = k/samples
            pt = (1-u)**2*A + 2*(1-u)*u*V + u**2*B
            out.append((float(pt[0]), float(pt[1])))
    out.append(chain[-1])
    return out

def _hex_chain(logo_scale, hex_round=0.0):
    """left-half chain of the intake hexagon, sized so the vertical gap to the
    Voron logo equals the logo-to-fan gap."""
    ys = [y for bar in _scale_logo(logo_scale) for _, y in bar]
    logo_top, logo_bot = min(ys), max(ys)
    gap = (FAN_C[1] - FAN_R) - logo_bot          # logo -> fan
    k = ((logo_top - gap) - HEX_C[1]) / HEX_H0   # match it above the logo
    cx, cy = HEX_C; W, H = HEX_W0*k, HEX_H0*k
    hexa = [(cx, cy-H), (cx-W/2, cy-H), (cx-W, cy), (cx-W/2, cy+H), (cx, cy+H)]
    return _round_corners(hexa, hex_round)

def draw(out_w=None, out_h=None, ss=4, logo_scale=1.25,
         smooth=2, smooth_to=300, two_tone=True, with_logo=True,
         hex_round=26.0, face_margin=18.0):
    if out_h is None: out_h = int(round(out_w * DESIGN_H / DESIGN_W))
    if out_w is None: out_w = int(round(out_h * DESIGN_W / DESIGN_H))
    scale = min(out_w / DESIGN_W, out_h / DESIGN_H)
    W = max(1, int(round(DESIGN_W*scale))); H = max(1, int(round(DESIGN_H*scale)))
    s = scale * ss * DESIGN_W / REF_W
    big = (W*ss, H*ss)

    body_c  = _smooth_top(BODY_L, smooth_to, smooth)
    face_c  = _inset_face(_smooth_top(FACE_L, smooth_to, smooth),
                          body_c, face_margin)
    body_p, face_p = _full(body_c), _full(face_c)
    hex_p   = _full(_hex_chain(logo_scale, hex_round))
    block_p = _full(BLOCK_L)

    m_body  = _mask(big, s, polys=[body_p])
    m_face  = _mask(big, s, polys=[face_p])
    m_hex   = _mask(big, s, polys=[hex_p])
    m_fan   = _mask(big, s, circles=[(FAN_C, FAN_R)])
    m_logo  = _mask(big, s, polys=_scale_logo(logo_scale))
    m_block = _mask(big, s, polys=[block_p])

    def down(a):
        return np.array(Image.fromarray((a*255).astype(np.uint8))
                        .resize((W, H), Image.NEAREST)) > 127
    m_body, m_face, m_hex, m_fan, m_logo, m_block = map(
        down, (m_body, m_face, m_hex, m_fan, m_logo, m_block))
    m_face &= m_body
    for m in (m_hex, m_fan, m_logo):
        m &= m_face
    m_block &= m_body

    out = np.zeros((H, W, 4), np.uint8)
    out[m_body]  = BODY if two_tone else FACE
    out[m_face]  = FACE
    out[m_hex]   = RECESS
    out[m_fan]   = RECESS
    if two_tone:
        out[_edge(m_face) & m_body] = OUTLINE
    out[_edge(m_hex) & m_face] = OUTLINE
    out[_edge(m_fan) & m_face] = OUTLINE
    out[m_block] = RECESS
    out[_edge(m_block) & m_body] = OUTLINE
    out[_edge(m_body)] = OUTLINE

    # the part is symmetric; force it exactly, before the (diagonal) logo
    half = W // 2
    out[:, W-half:] = out[:, :half][:, ::-1]
    if with_logo:
        out[m_logo] = OUTLINE
    return Image.fromarray(out, 'RGBA')

if __name__ == '__main__':
    import sys
    out = sys.argv[1] if len(sys.argv) > 1 else 'stealthburner.png'
    h = int(sys.argv[2]) if len(sys.argv) > 2 else 220
    draw(out_h=h).save(out)
    print(f'wrote {out}')
