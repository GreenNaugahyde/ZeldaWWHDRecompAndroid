#!/usr/bin/env python3
"""Seamless houses: the lighting tables of an interior's stage, for the stage that takes it in as a room.
build_env.py SRC_Stage.szs OUT.env: EnvR, Colo, Pale and Virt of SRC's stage data (stage.dzs inside Stage.bfres), as
    'SHEV' u32 version 1, then per table (in that order) u32 count, u32 entry size, the entries.
The runtime (mods/seamless_houses.cpp) appends Colo / Pale / Virt to the host stage's tables (their indices moved) and
gives the new rooms the first EnvR entry, so they are lit as in their own stage."""
import struct, sys
from arc import yaz0, sarc
from dzx import tables

SIZE = {'EnvR': 8, 'Colo': 12, 'Pale': 108, 'Virt': 56}  # HD entry sizes (from the stage files' spacing)
files = sarc(yaz0(open(sys.argv[1], 'rb').read()))
b = [v for k, v in files.items() if k.endswith('.bfres')][0]
for o, ch in tables(b):
    names = {c: (n, off) for c, n, off in ch}
    if 'EnvR' in names:
        break
else:
    raise SystemExit('no EnvR table')
out = b'SHEV' + struct.pack('>I', 1)
for c in ('EnvR', 'Colo', 'Pale', 'Virt'):
    n, off = names[c]
    out += struct.pack('>II', n, SIZE[c]) + b[o + off:o + off + n * SIZE[c]]
    print(c, n)
open(sys.argv[2], 'wb').write(out)
