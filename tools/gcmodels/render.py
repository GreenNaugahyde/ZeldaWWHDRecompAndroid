# A GameCube model drawn in its bind pose from the front and the side with its textures (first texture of each
# material, alpha test at 128 where the material has one), for comparing a conversion with the pack's preview.
#   python3 render.py Link.arc cl out.png [--shapes] (shapes in colours instead of textures)
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gx_texture  # noqa: E402
import j3d  # noqa: E402


def render(m, W=500, H=700, view=0, flat=False):
    tex = gx_texture.textures(m.c['TEX1'])
    img = np.zeros((H, W, 3), np.float32) + 0.8
    depth = np.full((H, W), -1e9)
    shape_of = np.full((H, W), -1)
    tris = []
    for si, s in enumerate(m.shapes):
        mat = s['material']
        ts = m.material_textures[mat] if mat >= 0 else []
        name = m.texture_names[ts[0]] if ts else None
        for t in s['tris']:
            P = [m.position(v) for v in t]
            UV = [m.arrays[j3d.TEX0][v[j3d.TEX0]] if j3d.TEX0 in v else (0, 0) for v in t]
            tris.append((si, name, P, UV))
    allp = np.array([p for _, _, P, _ in tris for p in P])
    lo, hi = allp.min(0), allp.max(0)
    sc = (H - 40) / (hi[1] - lo[1])
    rng = np.random.default_rng(1)
    cols = rng.uniform(0.2, 0.9, (len(m.shapes), 3))
    cache = {}
    for si, name, P, UV in tris:
        P = np.array(P)
        x = (P[:, 0] if view == 0 else -P[:, 2]) * sc + W / 2
        z = P[:, 2] if view == 0 else P[:, 0]
        y = H - 20 - (P[:, 1] - lo[1]) * sc
        x0, x1 = int(max(0, np.floor(x.min()))), int(min(W - 1, np.ceil(x.max())))
        y0, y1 = int(max(0, np.floor(y.min()))), int(min(H - 1, np.ceil(y.max())))
        if x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        d = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2])
        if abs(d) < 1e-9:
            continue
        a = ((y[1] - y[2]) * (gx - x[2]) + (x[2] - x[1]) * (gy - y[2])) / d
        b = ((y[2] - y[0]) * (gx - x[2]) + (x[0] - x[2]) * (gy - y[2])) / d
        c = 1 - a - b
        inside = (a >= 0) & (b >= 0) & (c >= 0)
        if not inside.any():
            continue
        zz = a * z[0] + b * z[1] + c * z[2]
        if flat or not name or name not in tex:
            col = np.broadcast_to(cols[si], gx.shape + (3,))
            alpha = np.ones(gx.shape)
        else:
            if name not in cache:
                f, w, h, px, _ = tex[name]
                cache[name] = np.array(px, np.float32).reshape(h, w, 4) / 255
            T = cache[name]
            u = a * UV[0][0] + b * UV[1][0] + c * UV[2][0]
            v = a * UV[0][1] + b * UV[1][1] + c * UV[2][1]
            tx = (np.floor((u % 1) * T.shape[1]).astype(int)) % T.shape[1]
            ty = (np.floor((v % 1) * T.shape[0]).astype(int)) % T.shape[0]
            s = T[ty, tx]
            col, alpha = s[..., :3], s[..., 3]
        ok = inside & (alpha >= 0.5)
        sub = depth[y0:y1 + 1, x0:x1 + 1]
        win = ok & (zz > sub)
        sub[win] = zz[win]
        img[y0:y1 + 1, x0:x1 + 1][win] = col[win]
        shape_of[y0:y1 + 1, x0:x1 + 1][win] = si
    return (np.clip(img, 0, 1) * 255).astype(np.uint8), shape_of


if __name__ == '__main__':
    arc = j3d.rarc(open(sys.argv[1], 'rb').read())
    m = j3d.Model(arc[sys.argv[2] + '.bdl'])
    flat = '--shapes' in sys.argv
    a, _ = render(m, view=0, flat=flat)
    b, _ = render(m, view=1, flat=flat)
    Image.fromarray(np.concatenate([a, b], 1)).save(sys.argv[3])
