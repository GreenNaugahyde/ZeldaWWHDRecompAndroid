#!/usr/bin/env python3
"""Merge shader head-start templates (WWHV, written by WWHD_TEMPLATE_EXPORT) from several devices.

Usage:
  shadermerge.py OUT IN1 [IN2 ...]   merge IN1, IN2, ... into OUT (OUT may also be one of the inputs)
  shadermerge.py --info IN ...       print what each template holds, nothing written

The format is in the comment above template_export (runtime/src/vk/vk_draw.cpp):
  "WWHV", u32 version 1, u32 raw size, zlib( u32 nfetch, {u32 size, bytes}*, u32 nstates,
  {u64 key, u64 program hash, u32 program size, u32 kind, i32 fetch, u32 vertex output mask,
   u32 nregs, (u32 reg, u32 value)*}*, u32 npipelines, PipelineRecipe* )

Merging:
  - fetch shaders are deduplicated by their bytes and the states' fetch indices remapped;
  - states are deduplicated by key, the first input's record wins;
  - pipelines are deduplicated by their bytes (all inputs must have the same PipelineRecipe size).
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


def read_template(path):
    data = open(path, 'rb').read()
    if len(data) < 12 or data[:4] != b'WWHV':
        raise SystemExit(f'{path}: not a WWHV template')
    version, raw_size = struct.unpack_from('<II', data, 4)
    if version != 1:
        raise SystemExit(f'{path}: version {version}, this tool reads 1')
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
    rest = len(raw) - p
    if npipes and rest % npipes:
        raise SystemExit(f'{path}: {rest} bytes for {npipes} pipelines')
    psize = rest // npipes if npipes else 0
    pipes = [raw[p + i * psize:p + (i + 1) * psize] for i in range(npipes)]
    return fetch, states, pipes, psize, len(data)


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
        fetch, states, pipes, psize, fsize = read_template(path)
        progs = {(s[2][8:16], s[2][16:20], s[2][20:24] == b'\x01\0\0\0') for s in states}
        print(f'{path}: {fsize} bytes; {len(pipes)} pipelines ({psize} bytes each), {len(states)} states, '
              f'{len(progs)} programs, {len(fetch)} fetch shaders')


def merge(out, paths):
    fetch, fetch_index = [], {}
    states, by_key, by_identity = [], {}, {}
    pipes, pipe_set = [], set()
    psize = None
    for path in paths:
        f, st, pp, ps, fsize = read_template(path)
        if pp:
            if psize is None:
                psize = ps
            elif ps != psize:
                raise SystemExit(f'{path}: pipelines of {ps} bytes, earlier inputs {psize}: '
                                 'templates from builds with another PipelineRecipe')
        new_fetch = new_states = new_pipes = other_regs = other_prog = key_drift = 0
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
        print(f'{path}: {len(pp)} pipelines, {len(st)} states, {len(f)} fetch shaders -> new: '
              f'{new_pipes} pipelines, {new_states} states, {new_fetch} fetch shaders; '
              f'same key, other registers: {other_regs} / other program: {other_prog}; same state, other key: {key_drift}')

    # pipelines whose shaders are missing can't come up (each input only keeps resolved ones)
    keys = set(by_key)
    missing = sum(1 for r in pipes if struct.unpack_from('<QQ', r)[0] not in keys
                  or struct.unpack_from('<QQ', r)[1] not in keys)
    raw = bytearray(struct.pack('<I', len(fetch)))
    for b in fetch:
        raw += struct.pack('<I', len(b)) + b
    raw += struct.pack('<I', len(states))
    for rec in states:
        raw += rec
    raw += struct.pack('<I', len(pipes))
    for r in pipes:
        raw += r
    comp = zlib.compress(bytes(raw), 9)
    with open(out, 'wb') as o:
        o.write(b'WWHV' + struct.pack('<II', 1, len(raw)) + comp)
    print(f'{out}: {len(pipes)} pipelines ({missing} with a shader missing), {len(states)} states, '
          f'{len(fetch)} fetch shaders; {len(raw)} bytes raw, {len(comp) + 12} written')


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == '--info':
        info(args[1:])
    elif len(args) >= 2 and not args[0].startswith('-'):
        merge(args[0], args[1:])
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
