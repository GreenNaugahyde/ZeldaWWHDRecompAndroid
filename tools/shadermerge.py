#!/usr/bin/env python3
"""Merge shader head-start templates (WWHV, written by WWHD_TEMPLATE_EXPORT) from several devices.

Usage:
  shadermerge.py OUT IN1 [IN2 ...]   merge IN1, IN2, ... into OUT (OUT may also be one of the inputs)
  shadermerge.py --info IN ...       print what each template holds, nothing written
  shadermerge.py --areas IN          the area tags: pipelines per area, shared ones, untagged ones

The format is in the comment above template_export (runtime/src/vk/vk_draw.cpp):
  "WWHV", u32 version 2, u32 raw size, zlib( u32 nfetch, {u32 size, bytes}*, u32 nstates,
  {u64 key, u64 program hash, u32 program size, u32 kind, i32 fetch, u32 vertex output mask,
   u32 nregs, (u32 reg, u32 value)*}*, u32 npipelines, u32 recipe size, PipelineRecipe*,
   u32 nareas, {u32 length, name}*, u32 ntags, {u64 recipe hash, u32 n, u32 area index*}*,
   u32 nlinks, {u32 area index, u32 area index}* )
Version 1 (read too) ends after the recipes, their size taken from the rest. Always written as version 2.

Merging:
  - fetch shaders are deduplicated by their bytes and the states' fetch indices remapped;
  - states are deduplicated by key, the first input's record wins;
  - pipelines are deduplicated by their bytes (all inputs must have the same PipelineRecipe size);
  - area tags (recipe hash -> areas, by name) are joined; a tag may name a recipe of another input:
    a device tags the head-start pipelines it drew, whose recipes only the main template has;
  - an input ending in .txt is an area graph (tools/areagraph.py: "area area" per line): its links
    replace those of the inputs before it.
Inputs come in order, so put the main template (private/shader-template/template.bin) first.

Checks printed per input:
  - "same key, other registers": the key came with other register values than in an earlier input. Normal:
    texture and sampler words (0xE000.., 0xF000..) change per draw and aren't part of the key;
  - "same key, other program": the key came with another program, stage, mask or fetch shader (should be 0);
  - "same state, other key": a state (program, registers, fetch shader) that an earlier input had
    under another key. Many of these mean the inputs come from builds whose shader keys differ; the
    runtime then says "the template doesn't fit this build" for part of the merged template.
The output holds no shader code, like its inputs; keep it outside the repo.
"""
import struct
import sys
import zlib


def hash_bytes(b, h=0x9E3779B97F4A7C15):
    """vk_draw.cpp hash_bytes: a recipe's hash in the area tags."""
    m = 0xFFFFFFFFFFFFFFFF
    n8 = len(b) // 8 * 8
    for (v,) in struct.iter_unpack('<Q', b[:n8]):
        h = ((h ^ v) * 0xFF51AFD7ED558CCD) & m
        h ^= h >> 32
    for c in b[n8:]:
        h = ((h ^ c) * 0x100000001B3) & m
    return h ^ (h >> 29)


def read_template(path):
    """fetch shaders, states, pipelines, recipe size, file size, tags {recipe hash: set of area names},
    links {(area name, area name)}"""
    data = open(path, 'rb').read()
    if len(data) < 12 or data[:4] != b'WWHV':
        raise SystemExit(f'{path}: not a WWHV template')
    version, raw_size = struct.unpack_from('<II', data, 4)
    if version not in (1, 2):
        raise SystemExit(f'{path}: version {version}, this tool reads 1 and 2')
    raw = zlib.decompress(data[12:])
    if len(raw) != raw_size:
        raise SystemExit(f'{path}: raw size {len(raw)}, header says {raw_size}')
    p = 0

    def u32():
        nonlocal p
        v, = struct.unpack_from('<I', raw, p)
        p += 4
        return v

    fetch = []
    for _ in range(u32()):
        n = u32()
        fetch.append(raw[p:p + n])
        p += n
    states = []  # (key, fetch index, record bytes with the fetch index field)
    for _ in range(u32()):
        key, prog, size, kind, fi, mask, nregs = struct.unpack_from('<QQIIiII', raw, p)
        end = p + 36 + nregs * 8
        if end > len(raw) or kind > 2 or fi >= len(fetch):
            raise SystemExit(f'{path}: bad state record at {p}')
        states.append((key, fi, raw[p:end]))
        p = end
    npipes = u32()
    tags, links = {}, set()
    if version == 1:
        rest = len(raw) - p
        if npipes and rest % npipes:
            raise SystemExit(f'{path}: {rest} bytes for {npipes} pipelines')
        psize = rest // npipes if npipes else 0
    else:
        psize = u32()
    pipes = [raw[p + i * psize:p + (i + 1) * psize] for i in range(npipes)]
    p += npipes * psize
    if version == 2:
        areas = []
        for _ in range(u32()):
            n = u32()
            areas.append(raw[p:p + n].decode('latin-1'))
            p += n
        for _ in range(u32()):
            h, = struct.unpack_from('<Q', raw, p)
            p += 8
            n = u32()
            tags.setdefault(h, set()).update(areas[i] for i in struct.unpack_from(f'<{n}I', raw, p))
            p += 4 * n
        for _ in range(u32() if p < len(raw) else 0):  # (exports of f330d669 end after the tags)
            a, b = struct.unpack_from('<II', raw, p)
            p += 8
            links.add((areas[a], areas[b]))
        if p != len(raw):
            raise SystemExit(f'{path}: {len(raw) - p} bytes left after the area tags')
    return fetch, states, pipes, psize, len(data), tags, links


def split_regs(ident):
    """state_identity without the registers (and their count): what must match for one key."""
    nregs, = struct.unpack_from('<I', ident, 20)
    return ident[:20] + ident[24 + nregs * 8:]


def state_identity(fetch, fi, rec):
    """The state without its key: program, kind, mask, registers (in any order) and the fetch shader's bytes."""
    regs = sorted(struct.iter_unpack('<II', rec[36:]))
    return rec[8:24] + rec[28:36] + struct.pack(f'<{2 * len(regs)}I', *(x for r in regs for x in r)) + \
        (fetch[fi] if fi >= 0 else b'')


def info(paths):
    for path in paths:
        fetch, states, pipes, psize, fsize, tags, links = read_template(path)
        progs = {(s[2][8:16], s[2][16:20], s[2][20:24] == b'\x01\0\0\0') for s in states}
        hashes = {hash_bytes(r) for r in pipes}
        areas = set().union(*tags.values()) if tags else set()
        print(f'{path}: {fsize} bytes; {len(pipes)} pipelines ({psize} bytes each), {len(states)} states, '
              f'{len(progs)} programs, {len(fetch)} fetch shaders; area tags: {len(areas)} areas, '
              f'{len(tags)} recipes ({len(hashes & tags.keys())} of these pipelines), {len(links)} links')


def areas_report(path):
    fetch, states, pipes, psize, fsize, tags, links = read_template(path)
    hashes = {hash_bytes(r) for r in pipes}
    per = {}
    for h, a in tags.items():
        if h in hashes:
            for name in a:
                per.setdefault(name, set()).add(h)
    tagged = hashes & tags.keys()
    print(f'{path}: {len(pipes)} pipelines, {len(tagged)} tagged in {len(per)} areas, {len(pipes) - len(tagged)} untagged')
    counts = {}
    for h in tagged:
        counts[len(tags[h])] = counts.get(len(tags[h]), 0) + 1
    print('pipelines by the number of areas they were drawn in: ' +
          ', '.join(f'{k}: {counts[k]}' for k in sorted(counts)[:12]) + (' ...' if len(counts) > 12 else ''))
    for share in (5, 10, 20):
        common = {h for h in tagged if len(tags[h]) >= share}
        print(f'  in {share}+ areas: {len(common)}')
    # as the runtime keeps them (vk_draw.cpp area head start, kCommonAreas 8): the common ones, plus the
    # area's and its neighbours' (template links)
    common = {h for h in tagged if len(tags[h]) >= 8}
    nb = {}
    for a, b in links:
        nb.setdefault(a, set()).add(b)
        nb.setdefault(b, set()).add(a)
    print(f'common (8+ areas): {len(common)}')
    print('area: its pipelines, without the common ones, with its neighbours (+ common: what is kept)')
    rows = []
    for name in per:
        around = set(per[name])
        for b in nb.get(name, ()):
            around |= per.get(b, set())
        rows.append((len(around | common), name, len(per[name]), len(per[name] - common), len(nb.get(name, ()))))
    for kept, name, n, own, nn in sorted(rows, reverse=True):
        print(f'  {name:12} {n:6} {own:6} {kept:7} ({nn} neighbours)')


def merge(out, paths):
    fetch, fetch_index = [], {}
    states, by_key, by_identity = [], {}, {}
    pipes, pipe_set = [], set()
    tags, links = {}, set()
    psize = None
    for path in paths:
        if path.endswith('.txt'):
            links = {tuple(l.split()) for l in open(path) if len(l.split()) == 2}
            print(f'{path}: {len(links)} area links')
            continue
        f, st, pp, ps, fsize, tg, ln = read_template(path)
        links |= ln
        if pp:
            if psize is None:
                psize = ps
            elif ps != psize:
                raise SystemExit(f'{path}: pipelines of {ps} bytes, earlier inputs {psize}: '
                                 'templates from builds with another PipelineRecipe')
        new_fetch = new_states = new_pipes = other_regs = other_prog = key_drift = new_tags = 0
        remap = []
        for b in f:
            if b not in fetch_index:
                fetch_index[b] = len(fetch)
                fetch.append(b)
                new_fetch += 1
            remap.append(fetch_index[b])
        for key, fi, rec in st:
            ident = state_identity(f, fi, rec)
            if key in by_key:
                if by_key[key] != ident:
                    if split_regs(by_key[key]) == split_regs(ident):
                        other_regs += 1
                    else:
                        other_prog += 1
                continue
            other = by_identity.get(ident)
            if other is not None and other != key:
                key_drift += 1
            by_key[key] = ident
            by_identity.setdefault(ident, key)
            nfi = remap[fi] if fi >= 0 else -1
            states.append(rec[:24] + struct.pack('<i', nfi) + rec[28:])
            new_states += 1
        for r in pp:
            if r not in pipe_set:
                pipe_set.add(r)
                pipes.append(r)
                new_pipes += 1
        for h, a in tg.items():
            have = tags.setdefault(h, set())
            new_tags += len(a - have)
            have |= a
        print(f'{path}: {len(pp)} pipelines, {len(st)} states, {len(f)} fetch shaders, {len(tg)} tagged recipes -> new: '
              f'{new_pipes} pipelines, {new_states} states, {new_fetch} fetch shaders, {new_tags} area tags; '
              f'same key, other registers: {other_regs} / other program: {other_prog}; same state, other key: {key_drift}')

    # pipelines whose shaders are missing can't come up (each input only keeps resolved ones)
    keys = set(by_key)
    missing = sum(1 for r in pipes if struct.unpack_from('<QQ', r)[0] not in keys
                  or struct.unpack_from('<QQ', r)[1] not in keys)
    hashes = {hash_bytes(r) for r in pipes}
    areas = sorted(set().union(*tags.values(), *links)) if tags or links else []
    area_index = {a: i for i, a in enumerate(areas)}
    raw = bytearray(struct.pack('<I', len(fetch)))
    for b in fetch:
        raw += struct.pack('<I', len(b)) + b
    raw += struct.pack('<I', len(states))
    for rec in states:
        raw += rec
    raw += struct.pack('<II', len(pipes), psize or 0)
    for r in pipes:
        raw += r
    raw += struct.pack('<I', len(areas))
    for a in areas:
        b = a.encode('latin-1')
        raw += struct.pack('<I', len(b)) + b
    raw += struct.pack('<I', len(tags))
    for h, a in tags.items():
        idx = sorted(area_index[x] for x in a)
        raw += struct.pack(f'<QI{len(idx)}I', h, len(idx), *idx)
    raw += struct.pack('<I', len(links))
    for a, b in sorted(links):
        raw += struct.pack('<II', area_index[a], area_index[b])
    comp = zlib.compress(bytes(raw), 9)
    with open(out, 'wb') as o:
        o.write(b'WWHV' + struct.pack('<II', 2, len(raw)) + comp)
    print(f'{out}: {len(pipes)} pipelines ({missing} with a shader missing), {len(states)} states, '
          f'{len(fetch)} fetch shaders, {len(areas)} areas, {len(hashes & tags.keys())} pipelines tagged '
          f'({len(tags)} tagged recipes), {len(links)} links; {len(raw)} bytes raw, {len(comp) + 12} written')


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--info':
        info(args[1:])
    elif len(args) == 2 and args[0] == '--areas':
        areas_report(args[1])
    elif len(args) >= 2 and not args[0].startswith('-'):
        merge(args[0], args[1:])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
