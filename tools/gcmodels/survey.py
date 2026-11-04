# How far each part of custom model packs is from HD's own (geometry: share of GameCube vertices near an HD one;
# textures: mean difference to HD's texture scaled down), over many packs: the near end of the spread is the game's original, so
# convert.py can tell what a pack changed. Writes baseline.json next to this file.
#   python3 survey.py HD_Link.bfres pack1/Link.arc pack2/Link.arc ...
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bc  # noqa: E402
import convert  # noqa: E402
import gx_texture  # noqa: E402
import j3d  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


class HD:
    def __init__(self, f):
        self.f = f
        self.tex = dict(f.group(f.section(1)))
        self.models = dict(f.group(f.section(0)))
        self.cache, self.points = {}, {}

    def image(self, n):
        if n not in self.cache:
            f, o = self.f, self.tex[n]
            w, h, fmt, sz, tm, sw = f.u32(o + 8), f.u32(o + 0xC), f.u32(o + 0x18), f.u32(o + 0x24), f.u32(o + 0x34), f.u32(o + 0x38)
            with tempfile.TemporaryDirectory() as t:
                open(t + '/i', 'wb').write(bytes(f.d[f.rel(o + 0xB0):f.rel(o + 0xB0) + sz]))
                open(t + '/m', 'wb').write(b'')
                subprocess.run([os.path.join(HERE, 'gx2tile'), 'untile', hex(fmt), str(w), str(h), '1', str(tm), hex(sw),
                                t + '/i', t + '/m', t + '/l'], capture_output=True, check=True)
                self.cache[n] = (fmt, bc.decode(fmt, w, h, open(t + '/l', 'rb').read()))
        return self.cache[n]

    def model_points(self, name):
        if name not in self.points:
            f, fm = self.f, self.models[name]
            bones, _ = convert.hd_skeleton(f, fm)
            vbl, pts = f.rel(fm + 16), []
            for _sn, so in f.group(f.rel(fm + 20)):
                skin, bone = f.u8(so + 0x16), bones[f.u16(so + 0x10)]
                vb = vbl + f.u16(so + 0x12) * 0x20
                al, bl, cnt = f.rel(vb + 0x10), f.rel(vb + 0x18), f.u32(vb + 8)
                off = {f.string(f.rel(al + k * 12)): f.u16(al + k * 12 + 6) for k in range(f.u8(vb + 4))}['_p0']
                st, data = f.u16(bl + 0xC), f.rel(bl + 0x14)
                for i in range(cnt):
                    p = struct.unpack_from('>3f', f.d, data + i * st + off)
                    pts.append(p if skin else j3d.mat_apply(bone['world'], p))
            self.points[name] = np.array(pts)
        return self.points[name]


def geometry_near(hd, name, gc):
    H = hd.model_points(name)
    G = np.array([gc.position(v) for s in gc.shapes for t in s['tris'] for v in t])
    if not len(G) or not len(H):
        return 100.0
    G = G[::max(1, len(G) // 400)]
    d = np.sqrt(((G[:, None, :] - H[None, :, :]) ** 2).sum(-1)).min(1)
    return float(100 * (d < 0.01 * (np.ptp(H, 0).max() or 1)).mean())


def texture_diff(hd, name, gfmt, w, h, px):
    fmt, hp = hd.image(name)
    g = np.array(px, np.float32).reshape(h, w, 4)
    hs = np.array(Image.fromarray(hp, 'RGBA').resize((w, h), Image.BOX), np.float32)
    if (fmt & 0x3F) in (0x34, 0x35):
        g = np.stack([g[:, :, 0], g[:, :, 3]], -1)[:, :, :2 if (fmt & 0x3F) == 0x35 else 1]
        hs = hs[:, :, :g.shape[2]]
    else:
        g, hs = g[:, :, :3], hs[:, :, :3]
    return float(np.abs(g - hs).mean())


def measure(hd, arc):
    geo, tex = {}, {}
    for name in hd.models:
        key = name + '.bdl' if name + '.bdl' in arc else name + '.bmd'
        if key not in arc:
            continue
        gc = j3d.Model(arc[key])
        geo[name] = geometry_near(hd, name, gc)
        if 'TEX1' in gc.c:
            for n, (gf, w, h, px, _) in gx_texture.textures(gc.c['TEX1']).items():
                if n in hd.tex and n not in tex:
                    tex[n] = texture_diff(hd, n, gf, w, h, px)
    return geo, tex


if __name__ == '__main__':
    hd = HD(convert.Bfres(open(sys.argv[1], 'rb').read()))
    geos, texs = {}, {}
    for p in sys.argv[2:]:
        g, t = measure(hd, j3d.rarc(open(p, 'rb').read()))
        for k, v in g.items():
            geos.setdefault(k, []).append(v)
        for k, v in t.items():
            texs.setdefault(k, []).append(v)
        print(os.path.basename(p), 'done', flush=True)
    base = {'packs': len(sys.argv) - 2,
            # (most packs keep most parts: the original is the near end of the spread)
            'geometry': {k: round(float(np.percentile(v, 75)), 1) for k, v in sorted(geos.items())},
            'texture': {k: round(float(np.percentile(v, 25)), 1) for k, v in sorted(texs.items())}}
    json.dump(base, open(os.path.join(HERE, 'baseline.json'), 'w'), indent=1)
    print(json.dumps({k: base['geometry'][k] for k in base['geometry'] if base['geometry'][k] < 95}))
