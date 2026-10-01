"""Index the GameCube decompilation (tww/): every assert -> the function that contains it.

Function boundaries come from a light-weight brace scan of the C++ sources (comments and
string literals are masked first), which is enough to attribute asserts to their function.
"""
import os
import re
import sys

ASSERT_RE = re.compile(r"\b(JUT_ASSERT|JUT_ASSERT_MSG|JUT_CONFIRM|JUT_ASSERT_DEBUG)\s*\(")
CONTROL = {"if", "for", "while", "switch", "catch", "return", "sizeof", "do", "else"}


def mask(src):
    """blank out comments and string/char literals (same length, newlines kept)"""
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j): out[k] = " "
            i = j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n": out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                if out[k] != "\n": out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def func_name(header):
    """'static void cBgS::Regist(cBgW* p) const' -> 'cBgS::Regist' (None if not a function)"""
    h = header.strip()
    p = h.find("(")
    if p <= 0 or ")" not in h[p:]:
        return None
    pre = h[:p].rstrip()
    m = re.search(r"((?:[A-Za-z_]\w*(?:<[^<>]*>)?::)*(?:~?[A-Za-z_]\w*|operator\s*\S+))$", pre)
    if not m:
        return None
    name = m.group(1)
    if name.split("::")[-1] in CONTROL or pre.split()[0] in ("struct", "class", "namespace", "enum", "union"):
        return None
    return name


def norm_cond(c):
    """canonical assert condition text (shared by both sides of the match)"""
    c = re.sub(r"\s+", "", c)
    c = re.sub(r"__generic\((.*?),,,__isnand,__isnanf,__isnanl,,,\)\(\1\)", r"isnan(\1)", c)
    c = re.sub(r"\bNULL\b", "0", c)
    c = re.sub(r"\bFALSE\b", "0", c)
    c = re.sub(r"\bTRUE\b", "1", c)
    c = c.replace("(0)", "0")
    return c


def split_args(s):
    """split at top-level commas"""
    out, depth, cur = [], 0, ""
    for c in s:
        if c in "([{": depth += 1
        elif c in ")]}": depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += c
    out.append(cur.strip())
    return out


def scan(path):
    raw = open(path, encoding="utf-8", errors="replace").read()
    src = mask(raw)
    funcs = []          # (name, start offset, end offset)
    stack = []          # per '{': function name or None
    last = 0            # start of the current header
    for i, c in enumerate(src):
        if c == "{":
            name = None
            if not any(stack):
                name = func_name(src[last:i].replace("\n", " "))
            stack.append((name, i))
            last = i + 1
        elif c == "}":
            if stack:
                name, start = stack.pop()
                if name:
                    funcs.append((name, start, i))
            last = i + 1
        elif c == ";":
            last = i + 1
    asserts = []
    for m in ASSERT_RE.finditer(src):
        # arguments from the raw text (the condition may contain strings)
        j, depth = m.end(), 1
        while j < len(raw) and depth:
            depth += {"(": 1, ")": -1}.get(raw[j], 0)
            j += 1
        args = split_args(raw[m.end():j - 1])
        if len(args) < 2:
            continue
        try:
            line = int(args[0], 0)
        except ValueError:
            line = None  # e.g. VERSION_SELECT(612, 613)
        cond = norm_cond(args[1])
        owner = None
        for name, a, b in funcs:
            if a <= m.start() <= b:
                owner = name
        asserts.append((line, cond, owner))
    return funcs, asserts


def index(root):
    out = {}  # (file basename, cond) -> set of function names
    nfiles = 0
    for d, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h", ".inc")):
                continue
            nfiles += 1
            _, asserts = scan(os.path.join(d, f))
            for line, cond, owner in asserts:
                if owner:
                    out.setdefault((f, cond), set()).add((owner, line))
    return out, nfiles


if __name__ == "__main__":
    idx, n = index(sys.argv[1])
    owners = {o for v in idx.values() for o, _ in v}
    print("files", n, "assert keys", len(idx), "functions with asserts", len(owners))
    for k in list(idx)[:5]:
        print(k, idx[k])


STR_RE = re.compile(rb'"((?:[^"\\\n]|\\.)*)"')


def _unescape(b):
    out, i = bytearray(), 0
    while i < len(b):
        c = b[i]
        if c == 0x5C and i + 1 < len(b):  # backslash
            n = b[i + 1:i + 2]
            m = {b"n": 10, b"t": 9, b"0": 0, b"\\": 0x5C, b'"': 0x22, b"'": 0x27, b"r": 13}.get(n)
            if m is not None:
                out.append(m); i += 2; continue
            if n == b"x":
                j = i + 2
                while j < len(b) and j < i + 4 and chr(b[j]) in "0123456789abcdefABCDEF": j += 1
                out.append(int(b[i + 2:j], 16) & 0xFF); i = j; continue
        out.append(c); i += 1
    return bytes(out)


def string_index(root):
    """string literal bytes -> set of (file basename, function name) using it"""
    out = {}
    for d, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith((".cpp", ".h", ".inc")):
                continue
            path = os.path.join(d, f)
            funcs, _ = scan(path)
            raw = open(path, "rb").read()
            # offsets in the decoded text and the raw bytes differ for non-ASCII; map via utf-8 decode
            text = raw.decode("utf-8", errors="replace")
            named = {}  # file-scope constant name -> literals (static const char x[] = "..."; tables of strings)
            for m in STR_RE.finditer(raw):
                lit = _unescape(m.group(1))
                if len(lit) < 3:
                    continue
                pos = len(raw[:m.start()].decode("utf-8", errors="replace"))
                owner = None
                for name, a, b in funcs:
                    if a <= pos <= b:
                        owner = name
                if owner:
                    out.setdefault(lit, set()).add((f, owner))
                else:
                    # the declaration this literal initialises: last '<ident>[...] =' or '<ident> =' before it
                    head = text[max(0, pos - 400):pos]
                    stmt = re.split(r";|}", head)[-1]
                    dm = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*=", stmt)
                    if dm:
                        named.setdefault(dm.group(1), []).append(lit)
            if named:
                masked = mask(text)
                for name, a, b in funcs:
                    body = masked[a:b]
                    for ident, lits in named.items():
                        if re.search(r"\b%s\b" % re.escape(ident), body):
                            for lit in lits:
                                out.setdefault(lit, set()).add((f, name))
    return out


FLOAT_RE = re.compile(r"(?<![\w.])(-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)f\b")


def float_key(v):
    """canonical float token: value rounded to float32 precision"""
    import struct
    try:
        return "F:%r" % struct.unpack(">f", struct.pack(">f", v))[0]
    except (OverflowError, struct.error):
        return "F:%r" % v


def float_index(root):
    """float literal token -> set of (file, function)"""
    out = {}
    for d, _, files in os.walk(os.path.join(root, "src")):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            path = os.path.join(d, f)
            funcs, _ = scan(path)
            text = mask(open(path, encoding="utf-8", errors="replace").read())
            for name, a, b in funcs:
                for m in FLOAT_RE.finditer(text, a, b):
                    v = float(m.group(1))
                    out.setdefault(float_key(abs(v)), set()).add((f, name))
    return out
