"""Register-agnostic shape of a PowerPC function, comparable across compilers (MWCC vs GHS).

Each instruction becomes its operation (primary opcode plus extended opcode), with registers and
immediates dropped; prologue/epilogue bookkeeping (stack frame, saving/restoring registers, mflr/
mtlr, register moves) is skipped because the two compilers do it differently. From that sequence:
  hist     multiset of operations
  bigrams  multiset of consecutive operation pairs
  cbr      conditional branches, loops (backward branches), compares, float ops
"""
import math
from collections import Counter

NAMES = {}


def op_key(w):
    op = w >> 26
    if op in (31, 19):
        return op * 2048 + ((w >> 1) & 0x3FF)
    if op in (59, 63):
        xo = (w >> 1) & 0x1F
        return op * 2048 + (xo if xo >= 16 else (w >> 1) & 0x3FF)
    return op


def skip(w):
    op = w >> 26
    ra = (w >> 16) & 31
    if w in (0x7C0802A6, 0x7C0803A6, 0x4E800020):          # mflr r0 / mtlr r0 / blr
        return True
    if op in (36, 37, 32, 54, 50, 47, 46) and ra == 1:       # stack saves/loads (stw/lwz/stfd/lfd/stmw/lmw r1)
        return True
    if op == 14 and ra == 1 and ((w >> 21) & 31) == 1:      # addi r1, r1, x
        return True
    if op == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 21) & 31) == ((w >> 11) & 31):   # mr
        return True
    if op == 63 and ((w >> 1) & 0x3FF) == 72:                 # fmr
        return True
    return False


class Shape:
    __slots__ = ("hist", "bigrams", "cbr", "loops", "cmps", "fops", "n", "hnorm", "bsum")


def shape(words, base=0):
    s = Shape()
    seq = [op_key(w) for w in words if not skip(w)]
    s.hist = Counter(seq)
    s.bigrams = Counter(zip(seq, seq[1:]))
    s.cbr = s.loops = s.cmps = s.fops = 0
    for i, w in enumerate(words):
        op = w >> 26
        if op == 16:
            s.cbr += 1
            bd = (w & 0xFFFC) - (0x10000 if w & 0x8000 else 0)
            if bd < 0:
                s.loops += 1
        elif op in (10, 11) or (op == 31 and ((w >> 1) & 0x3FF) in (0, 32)) or (op == 63 and ((w >> 1) & 0x3FF) in (0, 32)):
            s.cmps += 1
        elif op in (59, 63):
            s.fops += 1
    s.n = len(seq)
    s.hnorm = math.sqrt(sum(v * v for v in s.hist.values())) or 1.0
    s.bsum = sum(s.bigrams.values())
    return s


def cosine(a, b, na=None, nb=None):
    if not a or not b:
        return 0.0
    if len(a) > len(b):
        a, b, na, nb = b, a, nb, na
    dot = sum(v * b[k] for k, v in a.items() if k in b)
    na = na or math.sqrt(sum(v * v for v in a.values()))
    nb = nb or math.sqrt(sum(v * v for v in b.values()))
    return dot / (na * nb)


def wjaccard(a, b, sa=None, sb=None):
    """sum(min)/sum(max) of two multisets (sa, sb: their totals, if known)"""
    if not a or not b:
        return 0.0
    if len(a) > len(b):
        a, b, sa, sb = b, a, sb, sa
    inter = sum(min(v, b[k]) for k, v in a.items() if k in b)
    uni = (sa if sa is not None else sum(a.values())) + (sb if sb is not None else sum(b.values())) - inter
    return inter / uni if uni else 0.0


def ldiff(x, y):
    return abs(math.log((x + 1) / (y + 1)))
