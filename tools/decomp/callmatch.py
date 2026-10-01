"""Call-graph matching against the GameCube binary (BinDiff-style propagation).

GameCube side: every call site has a relocation naming its target, so the call graph is exact and
named. WWHD side: bl targets. Starting from trusted pairs, an unmatched WWHD function w and GameCube
function g are paired when w is called by / calls functions already paired with g's callers /
callees, and the pairing is the mutual best with a clear margin. Float/string tokens, size and call
counts break ties. Repeats until no new pairs appear.
"""
import math
from collections import Counter, defaultdict

from features import similarity


def build_graphs(G, W, wcallees):
    gkey = {}
    for g in G:
        gkey.setdefault(g.name, g)  # mangled names are unique enough; first definition wins
    gcallees = {g.name: [c for c in g.callees if c in gkey] for g in gkey.values()}
    gcallers = defaultdict(set)
    for a, cs in gcallees.items():
        for c in cs:
            gcallers[c].add(a)
    wcallers = defaultdict(set)
    for a, cs in wcallees.items():
        for c in cs:
            wcallers[c].add(a)
    return gkey, gcallees, gcallers, wcallers


def propagate(G, W, wcallees, seeds, idf, rounds=20, log=print):
    """seeds: {wwhd addr: gc mangled name}. Returns the grown mapping."""
    gkey, gcallees, gcallers, wcallers = build_graphs(G, W, wcallees)
    m = {w: g for w, g in seeds.items() if g in gkey}
    rev = {g: w for w, g in m.items()}
    for r in range(rounds):
        # candidate pairs: neighbours of matched pairs, scored by shared matched neighbours
        score = Counter()
        for w, g in list(m.items()):
            for wn, gn, rel in ((wcallees.get(w, ()), gcallees.get(g, ()), "callee"),
                                (wcallers.get(w, ()), gcallers.get(g, ()), "caller")):
                wu = [a for a in set(wn) if a not in m]
                gu = [b for b in set(gn) if b not in rev]
                if not wu or not gu or len(wu) * len(gu) > 400:
                    continue
                for a in wu:
                    for b in gu:
                        score[(a, b)] += 1.0
        if not score:
            break
        # add content similarity as a tie-breaker and pick mutual bests
        best_w, best_g = {}, {}
        full = {}
        for (a, b), s in score.items():
            sim = similarity(gkey[b], W[a], idf)
            gs, ws = gkey[b], W[a]
            size_ok = (min(gs.size, ws.size) + 32) / (max(gs.size, ws.size) + 32)
            v = s + 2.0 * sim + 0.5 * size_ok
            full[(a, b)] = v
            if v > best_w.get(a, (0, None))[0]:
                best_w[a] = (v, b)
            if v > best_g.get(b, (0, None))[0]:
                best_g[b] = (v, a)
        # margin: second best for the same w must be clearly lower
        second = defaultdict(float)
        for (a, b), v in full.items():
            if best_w[a][1] != b:
                second[a] = max(second[a], v)
        added = 0
        for a, (v, b) in best_w.items():
            if best_g[b][1] == a and v >= 2.0 and v >= 1.5 * second[a] and b not in rev and a not in m:
                m[a] = b
                rev[b] = a
                added += 1
        log("round %d: +%d (total %d)" % (r + 1, added, len(m)))
        if not added:
            break
    return m
