#!/usr/bin/env python3
"""Audit Link's procedures (GameCube source) for inline per-step code that the shared primitives
do not cover: compound assignments / increments on members, direct speed/position changes.
usage: proc_audit.py  -> per proc: inline per-step statements (file:line)"""
import glob, re
SRC = sorted(glob.glob('tww/src/d/actor/d_a_player_main.cpp') + glob.glob('tww/src/d/actor/d_a_player_*.inc'))
FN = re.compile(r'^\S.*\bdaPy_lk_c::(proc\w+)\(\)\s*\{', re.M)
OPS = re.compile(r'(\b(?:m\w+|speed\w*|current\.\w+(?:\.\w)?|shape_angle\.\w|gravity|old\.\w+)(?:\.\w+|\[\w+\])*)\s*(\+=|-=|\*=|/=)|'
                 r'(\b(?:m\w+)(?:\.\w+)*)(\+\+|--)|(\+\+|--)(m\w+(?:\.\w+)*)')
procs = {}
for path in SRC:
    text = open(path).read()
    for m in FN.finditer(text):
        name = m.group(1)
        if name.endswith('_init'):
            continue
        start = m.end()
        depth, i = 1, start
        while depth and i < len(text):
            depth += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        body = text[start:i]
        line0 = text[:start].count('\n') + 1
        hits = []
        for k, line in enumerate(body.split('\n')):
            if OPS.search(line):
                hits.append('%s:%d: %s' % (path.split('/')[-1], line0 + k, line.strip()))
        procs[name] = hits
for name in sorted(procs, key=lambda n: len(procs[n])):
    print('%-28s %d inline per-step statements' % (name, len(procs[name])))
    for h in procs[name]:
        print('    ' + h)
