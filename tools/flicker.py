#!/usr/bin/env python3
"""Find flicker in consecutive frame dumps: frame n differs from both neighbours
while the neighbours agree. Usage: flicker.py FIRST LAST [dir]"""
import sys
import numpy as np
from PIL import Image

first, last = int(sys.argv[1]), int(sys.argv[2])
d = sys.argv[3] if len(sys.argv) > 3 else "."
load = lambda n: np.asarray(Image.open(f"{d}/frame_{n}.png").convert("RGB"), dtype=np.int16)
frames = {n: load(n) for n in range(first, last + 1)}
for n in range(first + 1, last):
    a, b, c = frames[n - 1], frames[n], frames[n + 1]
    ab = np.abs(a - b).max(axis=2)
    bc = np.abs(b - c).max(axis=2)
    ac = np.abs(a - c).max(axis=2)
    spike = (ab > 40) & (bc > 40) & (ac < 12)
    cnt = int(spike.sum())
    print(f"{n}: diff prev {np.mean(ab):5.2f} next {np.mean(bc):5.2f} | spike px {cnt}", end="")
    if cnt > 50:
        ys, xs = np.nonzero(spike)
        print(f"  bbox x {xs.min()}-{xs.max()} y {ys.min()}-{ys.max()}", end="")
    print()
