# Every model of the game that can be held as a loot sword (notes/loot.md): the game's archives (content/Common/Object
# and the archives inside content/Common/Pack/*.pack) gone through; per model the shapes of its main texture (the one
# with the most triangles: a loot sword shows one picture), kept when it is neither tiny nor huge nor too detailed.
# Writes runtime/src/gcmodel/hd_sword_found.inc (SwordDef rows: its own main direction, the handle at its thinner end,
# size and strength from its real size).
#   python3 loot_survey.py GAME_DIR OUT.inc
import hashlib
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nintendo as nin  # noqa: E402
from convert import Bfres  # noqa: E402

GOOD_FORMATS = {0x31, 0x32, 0x33, 0x34, 0x35}
SKIP_PREFIX = ('sea_', 'Link', 'LkAnm', 'LkD', 'System', 'Always', 'Dalways', 'HomeBtn', 'Tlogo', 'PName', 'Name_',
               'Grid', 'Search', 'Mii_')


def archives(game):
    obj = os.path.join(game, 'content/Common/Object')
    for n in sorted(os.listdir(obj)):
        if n.endswith('.szs'):
            yield n, open(os.path.join(obj, n), 'rb').read()
    pk = os.path.join(game, 'content/Common/Pack')
    for p in sorted(os.listdir(pk)):
        if not p.endswith('.pack') or 'permanent' not in p:
            continue
        files = nin.sarc(open(os.path.join(pk, p), 'rb').read())
        for n in sorted(files):
            if n.endswith('.szs') and not os.path.exists(os.path.join(obj, n)):
                yield n, files[n]


def shape_info(f, fm, si, so):
    vb = f.rel(fm + 16) + f.u16(so + 0x12) * 0x20
    al, bl = f.rel(vb + 0x10), f.rel(vb + 0x18)
    attrs = {f.string(f.rel(al + k * 12)): (f.u16(al + k * 12 + 6), f.u32(al + k * 12 + 8)) for k in range(f.u8(vb + 4))}
    if not all(a in attrs for a in ('_p0', '_n0', '_u0')) or attrs['_p0'][1] not in (0x811, 0x813):
        return None
    n, st, data = f.u32(vb + 8), f.u16(bl + 0xC), f.rel(bl + 0x14)
    mesh = f.rel(so + 0x24)
    if f.u32(mesh) != 4:
        return None
    P = np.frombuffer(bytes(f.d[data:data + n * st]), dtype='>f4').reshape(n, st // 4)[:, attrs['_p0'][0] // 4:attrs['_p0'][0] // 4 + 3].astype(np.float64)
    return P, f.u32(mesh + 8) // 3, f.u8(so + 0x16)


def bone_world(f, fm, index):
    sk = f.rel(fm + 12)
    bones = {}
    for name, bo in f.group(f.rel(sk + 0x10)):
        flags = f.u32(bo + 0x10)
        s, r, t = f.f32s(bo + 0x14, 3), f.f32s(bo + 0x20, 4), f.f32s(bo + 0x30, 3)
        from convert import euler_matrix, quat_matrix
        m = euler_matrix(s, r[:3], t) if flags & 0x1000 else quat_matrix(s, r, t)
        bones[f.u16(bo + 4)] = (f.s16(bo + 6), np.array(m + [[0, 0, 0, 1]]))
    w = np.eye(4)
    i, guard = index, 0
    while i in bones and guard < 64:
        w = bones[i][1] @ w
        i, guard = bones[i][0], guard + 1
    return w


def main():
    game, out = sys.argv[1:3]
    rows, seen = [], set()
    for an, data in archives(game):
        if an.startswith(SKIP_PREFIX):
            continue
        try:
            sz = nin.yaz0(data) if data[:4] == b'Yaz0' else data
            files = nin.sarc(sz)
        except Exception:
            continue
        for bn, b in files.items():
            if not bn.endswith('.bfres') or len(b) < 0x40 or b[:4] != b'FRES':
                continue
            try:
                f = Bfres(b)
                tex = {n: f.u32(o + 0x18) & 0x3F for n, o in f.group(f.section(1))}
                models = f.group(f.section(0))
            except Exception:
                continue
            for mn, fm in models:
                try:
                    mats = [(n, o) for n, o in f.group(f.rel(fm + 24))]
                    shapes = f.group(f.rel(fm + 20))
                except Exception:
                    continue
                per_tex = {}
                for si, (sn, so) in enumerate(shapes):
                    try:
                        mo = mats[f.u16(so + 0xE)][1]
                        t = f.string(f.rel(f.rel(mo + 0x28))) if f.u8(mo + 0x11) else None
                        info = shape_info(f, fm, si, so)
                    except Exception:
                        continue
                    if not t or t not in tex or tex[t] not in GOOD_FORMATS or info is None:
                        continue
                    P, tris, skin = info
                    if skin == 0:
                        w = bone_world(f, fm, f.u16(so + 0x10))
                        P = (np.c_[P, np.ones(len(P))] @ w.T)[:, :3]
                    per_tex.setdefault(t, []).append((si, P, tris))
                if not per_tex:
                    continue
                t, parts = max(per_tex.items(), key=lambda kv: sum(p[2] for p in kv[1]))
                parts = parts[:7]
                P = np.vstack([p[1] for p in parts])
                tris = sum(p[2] for p in parts)
                if tris < 8 or len(P) > 3000 or not np.isfinite(P).all():
                    continue
                if len(P) < 12:
                    continue
                c = P - P.mean(0)
                try:
                    vt = np.linalg.svd(c, full_matrices=False)[2]
                except Exception:
                    continue
                along = c @ vt[0]
                length = float(np.ptp(along))
                if length < 15 or length > 1200:
                    continue
                # (a flat card: effects, billboards, door panels)
                if float(np.ptp(c @ vt[2])) < max(1.0, 0.03 * length) and len(P) < 40:
                    continue
                # (pieces that only come together when animated: the biggest connected piece much smaller than all)
                key_of = {}
                par = list(range(len(P)))
                def find(a):
                    while par[a] != a:
                        par[a] = par[par[a]]
                        a = par[a]
                    return a
                for i, p in enumerate(np.round(P, 1)):
                    k = tuple(p)
                    if k in key_of:
                        par[find(i)] = find(key_of[k])
                    else:
                        key_of[k] = i
                base = 0
                for si, Pp, _ in parts:
                    so = shapes[si][1]
                    try:
                        mesh = f.rel(so + 0x24)
                        ifmt, cnt = f.u32(mesh + 4), f.u32(mesh + 8)
                        ib = f.rel(f.rel(mesh + 0x14) + 0x14)
                        idx = struct.unpack_from('>%d%s' % (cnt, 'I' if ifmt == 9 else 'H'), f.d, ib)
                        for i in range(0, cnt - 2, 3):
                            a, b2, c2 = idx[i] + base, idx[i + 1] + base, idx[i + 2] + base
                            if max(a, b2, c2) < len(P):
                                par[find(b2)] = find(a)
                                par[find(c2)] = find(a)
                    except Exception:
                        pass
                    base += len(Pp)
                groups = {}
                for i in range(len(P)):
                    groups.setdefault(find(i), []).append(i)
                big = max(groups.values(), key=len)
                diag_all = float(np.linalg.norm(np.ptp(P, 0)))
                diag_big = float(np.linalg.norm(np.ptp(P[big], 0)))
                if diag_big < 0.6 * diag_all:
                    continue
                key = (an, mn, len(P), t)
                if key in seen:
                    continue
                seen.add(key)
                r = length / 103.0
                scale = min(1.6, max(0.4, math.sqrt(r) * 0.9))
                power = int(min(220, max(20, 25 + 55 * min(r, 3.0))))
                sid = 'f' + hashlib.md5(('%s/%s/%s' % (an, bn, mn)).encode()).hexdigest()[:10]
                rows.append((sid, '%s %s' % (an[:-4], mn), an, bn, mn, [p[0] for p in parts], t, scale, power, length, len(P)))
    with open(out, 'w') as o:
        o.write('// Generated by tools/gcmodels/loot_survey.py: every model of the game that can be held (loot swords, found\n')
        o.write('// things): its main texture\'s shapes, its own main direction, the handle at its thinner end. Do not edit.\n')
        for sid, name, an, bn, mn, shapes, t, scale, power, length, nv in rows:
            sh = ', '.join(str(x) for x in shapes) + ', -1'
            nm = name.replace('"', '')
            o.write(f'{{"{sid}", "{nm}", "{an}", "{bn}", "{mn}", {{{sh}}}, false, -1, -1, 0.25f, {scale:.2f}f, false, "{t}", {{-1, 0, 0}}, {power}}},'
                    f'  // {length:.0f} long, {nv} vertices\n')
    print(len(rows), 'things')


if __name__ == '__main__':
    main()
