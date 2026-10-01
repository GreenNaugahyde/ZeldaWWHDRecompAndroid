"""Fit match2's pair-scoring weights (conditional logit with a 'no match' option).

Positives: trusted pairs (assert/strings/profile seeds plus function-pointer tables). For each,
the candidate set is the GC functions of the same source file plus call-graph neighbours of
matched functions; in a third of the examples the true function is removed so the model learns
when to say 'none of these'. Prints the weights to paste into match2.Matcher.WEIGHTS and a
cross-validated precision table.

Usage: train.py names.tsv cking.rpx tww
"""
import bisect
import random
import sys
from collections import defaultdict

import numpy as np

from binmodel import GC, WW, refine_functions
from heldout import load_rows, resolve_seeds
from match2 import Matcher
from xref import Xref


def build(m, truth, rnd, max_cands=40):
    g = m.g
    byfile = defaultdict(list)
    for f in g.funcs:
        if m.canon[f.id] == f.id:
            byfile[f.file].append(f.id)
    m.tu_labels()
    m.learn_data()
    m.learn_fields()
    lab_addr = m.lab_addr
    X, groups = [], []
    for a, c in truth.items():
        # file window from the labelled neighbours, excluding a itself
        i = bisect.bisect_left(lab_addr, a)
        lo = i - 1
        hi = i + 1 if i < len(lab_addr) and lab_addr[i] == a else i
        tu = None
        if lo >= 0 and hi < len(lab_addr) and m.lab[lo][1] == m.lab[hi][1]:
            tu = (frozenset((m.lab[lo][1],)), m.lab[lo][2], m.lab[hi][2])
        cands = set(byfile[g.funcs[c].file])
        for t in list(m.wcallees.get(a, ())) + list(m.wcallers.get(a, ())):
            if t in m.M:
                for n in list(m.gcallees.get(m.M[t], ()))[:30] + list(m.gcallers.get(m.M[t], ()))[:30]:
                    cands.add(n)
        cands.discard(c)
        cands = rnd.sample(sorted(cands), min(len(cands), max_cands))
        cands += [rnd.randrange(len(g.funcs)) for _ in range(8)]   # decoys from anywhere
        cands = [m.canon[x] for x in cands if m.canon[x] != c]
        drop = rnd.random() < 0.33
        if not drop:
            cands.append(c)
        rows = [m.feats(a, x, tu) for x in cands]
        label = cands.index(c) if not drop else -1
        groups.append((len(X), len(rows), label))
        X.extend(rows)
    return np.array(X), groups


def fit(X, groups, l2=1e-3, iters=600, lr=0.5):
    n = X.shape[1]
    w = np.zeros(n)
    for it in range(iters):
        s = X @ w
        grad = np.zeros(n)
        for st, k, lab in groups:
            ss = s[st:st + k]
            mx = max(0.0, ss.max())
            e = np.exp(ss - mx)
            z = np.exp(-mx) + e.sum()
            p = e / z
            grad += (p[:, None] * X[st:st + k]).sum(0)
            if lab >= 0:
                grad -= X[st + lab]
        grad = grad / len(groups) + l2 * w
        w -= lr * grad
    return w


def evaluate(X, groups, w):
    s = X @ w
    out = []
    for st, k, lab in groups:
        ss = s[st:st + k]
        mx = max(0.0, ss.max())
        e = np.exp(ss - mx)
        p = e / (np.exp(-mx) + e.sum())
        j = int(ss.argmax())
        out.append((p[j], j == lab))
    for th in (0.5, 0.7, 0.8, 0.9, 0.95):
        sel = [ok for p, ok in out if p >= th]
        print("  p>=%.2f: named %5d of %5d, precision %.1f%%" % (th, len(sel), len(out),
                                                                 100.0 * sum(sel) / max(1, len(sel))))


def main():
    tsv, rpx, tww = sys.argv[1:4]
    g = GC(tww)
    x = Xref(rpx)
    refine_functions(x)
    w = WW(x)
    m = Matcher(g, w, log=lambda s: None)
    seeds = resolve_seeds(m, load_rows(tsv), {"assert", "strings", "profile"})
    for a, c in seeds.items():
        m.add(a, c, "seed")
    while m.stage_vtables():
        pass
    import os
    if os.environ.get("TRAIN_FROM"):
        # wider truth: an earlier run's high-confidence pairs (wwhd_to_gc.tsv), excluding the legacy stage
        for line in list(open(os.environ["TRAIN_FROM"]))[1:]:
            p = line.split("\t")
            if p[5] in ("graph", "tu", "vtable") and int(p[0], 16) not in m.M:
                ids = g.byname.get(p[1])
                if ids:
                    m.add(int(p[0], 16), m.canon[ids[0]], p[5])
    # skip wrappers whose callee was inlined in WWHD (the body is the callee's; ambiguous label)
    truth = {a: c for a, c in m.M.items() if m.w.size[a] >= 24 and m.w.size[a] <= 3 * m.g.funcs[c].size + 64}
    print("truth pairs", len(truth))
    rnd = random.Random(7)
    keys = sorted(truth)
    rnd.shuffle(keys)
    keys = keys[:int(os.environ.get("TRAIN_MAX", "100000"))]
    half = len(keys) // 2
    A = {k: truth[k] for k in keys[:half]}
    B = {k: truth[k] for k in keys[half:]}
    XA, gA = build(m, A, rnd)
    XB, gB = build(m, B, rnd)
    wA = fit(XA, gA)
    print("cross-validated (fit on half A, test on half B):")
    evaluate(XB, gB, wA)
    X = np.vstack([XA, XB])
    gs = gA + [(st + len(XA), k, lab) for st, k, lab in gB]
    wt = fit(X, gs)
    print("WEIGHTS = (%s)" % ", ".join("%.3f" % v for v in wt))
    for name, v in zip(Matcher.FEATURES, wt):
        print("  %-14s %7.3f" % (name, v))


if __name__ == "__main__":
    main()
