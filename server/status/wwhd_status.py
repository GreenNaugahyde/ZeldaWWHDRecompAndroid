#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""wwhd-status: the online server's status page behind a login (fork; docs/online-server-vps.md).

Reads the server's read-only status socket (files.status_socket in server.toml) and shows players, where they are,
parties and the server's counts. Python 3.9+ standard library only, no packages to install.

  wwhd_status.py serve   [--socket PATH] [--users FILE] [--bind 127.0.0.1] [--port 8780] [--insecure-cookies]
  wwhd_status.py adduser NAME [--users FILE]     (asks for the password twice)
  wwhd_status.py passwd  NAME [--users FILE]
  wwhd_status.py deluser NAME [--users FILE]
  wwhd_status.py users        [--users FILE]

It listens on 127.0.0.1 only by default: put a TLS reverse proxy in front (server/deploy/Caddyfile). Passwords are
kept as scrypt hashes; sessions live in memory (a restart logs everyone out). Failed logins are logged as
"wwhd-status: login failed user=<name> ip=<address>" for fail2ban (server/deploy/fail2ban).
"""
import argparse
import base64
import getpass
import hashlib
import hmac
import html
import http.server
import ipaddress
import json
import os
import re
import secrets
import socket
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")

SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 15, 8, 1     # ~32 MB and ~0.1 s per hash
HASH_SLOTS = threading.BoundedSemaphore(2)      # at most two hashes at once (memory)
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]{1,32}$")
MIN_PASSWORD = 12

SESSION_IDLE_S = 30 * 60
SESSION_MAX_S = 12 * 3600
MAX_SESSIONS = 200
FAIL_WINDOW_S = 15 * 60
FAIL_PER_IP = 5          # failed logins per address in the window, then that address waits
FAIL_PER_USER = 10       # failed logins per user name in the window, then that name waits
STATUS_CACHE_S = 2.0
MAX_BODY = 4096
MAX_STATUS_BYTES = 16 * 1024 * 1024


def log(msg):
    sys.stderr.write("wwhd-status: %s\n" % msg)
    sys.stderr.flush()


# ------------------------------------------------------------------------------------------------ users

def hash_password(pw):
    salt = secrets.token_bytes(16)
    with HASH_SLOTS:
        h = hashlib.scrypt(pw.encode(), salt=salt, n=SCRYPT_N, r=SCRYPT_R, p=SCRYPT_P, maxmem=64 << 20, dklen=32)
    b64 = lambda b: base64.b64encode(b).decode()
    return "scrypt$%d$%d$%d$%s$%s" % (SCRYPT_N, SCRYPT_R, SCRYPT_P, b64(salt), b64(h))


def check_password(pw, stored):
    try:
        kind, n, r, p, salt, want = stored.split("$")
        if kind != "scrypt":
            return False
        n, r, p = int(n), int(r), int(p)
        if n > 2 ** 17 or r > 16 or p > 4:
            return False
        salt, want = base64.b64decode(salt), base64.b64decode(want)
    except ValueError:
        return False
    with HASH_SLOTS:
        got = hashlib.scrypt(pw.encode(), salt=salt, n=n, r=r, p=p, maxmem=128 << 20, dklen=len(want))
    return hmac.compare_digest(got, want)


# a hash to check unknown names against, so that a wrong name takes as long as a wrong password
DUMMY_HASH = None


class Users:
    def __init__(self, path):
        self.path = path
        self.mtime = None
        self.users = {}
        self.lock = threading.Lock()

    def _load(self):
        try:
            st = os.stat(self.path)
        except FileNotFoundError:
            self.users, self.mtime = {}, None
            return
        if st.st_mtime_ns == self.mtime:
            return
        if st.st_mode & 0o077:
            log("warning: %s can be read by other users; chmod 600 it" % self.path)
        with open(self.path, encoding="utf-8") as f:
            self.users = json.load(f).get("users", {})
        self.mtime = st.st_mtime_ns

    def get(self, name):
        with self.lock:
            self._load()
            return self.users.get(name)

    def all(self):
        with self.lock:
            self._load()
            return dict(self.users)

    def save(self, users):
        tmp = self.path + ".new"
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump({"users": users}, f, indent=1, sort_keys=True)
        os.replace(tmp, self.path)


def ask_password():
    while True:
        a = getpass.getpass("password (at least %d characters): " % MIN_PASSWORD)
        if len(a) < MIN_PASSWORD:
            print("too short")
            continue
        if getpass.getpass("again: ") != a:
            print("not the same")
            continue
        return a


# ------------------------------------------------------------------------------------------------ the server's data

class Status:
    def __init__(self, sock_path):
        self.path = sock_path
        self.lock = threading.Lock()
        self.at = 0.0
        self.data = None
        with open(os.path.join(HERE, "names.json"), encoding="utf-8") as f:
            n = json.load(f)
        self.stages, self.sea = n["stages"], n["sea"]

    def _ask(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3)
        try:
            s.connect(self.path)
            s.sendall(b"status-json\n")
            chunks, size = [], 0
            while True:
                b = s.recv(65536)
                if not b:
                    break
                chunks.append(b)
                size += len(b)
                if size > MAX_STATUS_BYTES:
                    raise ValueError("answer too long")
        finally:
            s.close()
        return json.loads(b"".join(chunks).decode("utf-8", "replace"))

    def place(self, p):
        stage = p.get("stage") or ""
        if not stage:
            return "Loading / title"
        if stage == "sea":
            sq = p.get("square", -1)
            island = self.sea[sq] if 0 <= sq < len(self.sea) else "open sea"
            return "Sea: " + island
        name = self.stages.get(stage, stage)
        return "%s (room %d)" % (name, p.get("room", 0)) if p.get("room", 0) else name

    def get(self):
        with self.lock:
            now = time.monotonic()
            if self.data is not None and now - self.at < STATUS_CACHE_S:
                return self.data
            try:
                d = self._ask()
                d["reachable"] = True
                for p in d.get("list", []):
                    p["place"] = self.place(p)
                    p["at_sea"] = p.get("stage") == "sea"
            except (OSError, ValueError) as e:
                d = {"reachable": False, "error": "the game server does not answer (%s)" % e.__class__.__name__}
            d["checked"] = int(time.time())
            self.data, self.at = d, now
            return d


# ------------------------------------------------------------------------------------------------ sessions + throttling

class Sessions:
    def __init__(self):
        self.lock = threading.Lock()
        self.s = {}

    def new(self, user, pw_hash):
        tok, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(24)
        now = time.time()
        with self.lock:
            self._sweep(now)
            if len(self.s) >= MAX_SESSIONS:  # the oldest goes
                del self.s[min(self.s, key=lambda k: self.s[k]["born"])]
            self.s[tok] = {"user": user, "hash": pw_hash, "csrf": csrf, "born": now, "seen": now}
        return tok

    def get(self, tok):
        if not tok:
            return None
        now = time.time()
        with self.lock:
            e = self.s.get(tok)
            if not e:
                return None
            if now - e["seen"] > SESSION_IDLE_S or now - e["born"] > SESSION_MAX_S:
                del self.s[tok]
                return None
            e["seen"] = now
            return e

    def drop(self, tok):
        with self.lock:
            self.s.pop(tok, None)

    def _sweep(self, now):
        for k in [k for k, e in self.s.items() if now - e["seen"] > SESSION_IDLE_S or now - e["born"] > SESSION_MAX_S]:
            del self.s[k]


class Throttle:
    def __init__(self):
        self.lock = threading.Lock()
        self.fails = {}

    def _recent(self, key, now):
        v = [t for t in self.fails.get(key, []) if now - t < FAIL_WINDOW_S]
        if v:
            self.fails[key] = v
        else:
            self.fails.pop(key, None)
        return v

    def wait_s(self, ip, user):
        """seconds this address / name must wait before another try (0: may try)"""
        now = time.time()
        with self.lock:
            w = 0
            for key, limit in (("ip:" + ip, FAIL_PER_IP), ("user:" + user, FAIL_PER_USER)):
                v = self._recent(key, now)
                if len(v) >= limit:
                    w = max(w, int(FAIL_WINDOW_S - (now - v[-limit])) + 1)
            return w

    def fail(self, ip, user):
        now = time.time()
        with self.lock:
            if len(self.fails) > 100000:
                self.fails.clear()
            for key in ("ip:" + ip, "user:" + user):
                self.fails.setdefault(key, []).append(now)

    def ok(self, ip, user):
        with self.lock:
            self.fails.pop("ip:" + ip, None)
            self.fails.pop("user:" + user, None)


# ------------------------------------------------------------------------------------------------ the web side

SECURITY_HEADERS = {
    "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
                               "connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'",
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    # same-origin, not no-referrer: with no-referrer browsers send "Origin: null" on our own form posts
    "Referrer-Policy": "same-origin",
    "Permissions-Policy": "camera=(), microphone=(), geolocation=(), interest-cohort=()",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
}

PAGE = """<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex, nofollow"><title>%(title)s</title>
<link rel="stylesheet" href="/static/app.css"></head>
<body class="%(cls)s">%(body)s</body></html>"""

LOGIN = """<main class="login"><form method="post" action="/login" autocomplete="on">
<h1>Server status</h1><p class="sub">Sign in to see who is online.</p>
%(msg)s
<label>User<input name="user" autocomplete="username" required maxlength="32" autofocus></label>
<label>Password<input name="password" type="password" autocomplete="current-password" required maxlength="256"></label>
<button type="submit">Sign in</button></form></main>"""

DASH = """<meta name="csrf" content="%(csrf)s">
<header><div><h1 id="srv-name">Server status</h1><span id="state" class="state">connecting</span></div>
<form method="post" action="/logout"><input type="hidden" name="csrf" value="%(csrf)s">
<span class="who">%(user)s</span><button type="submit" class="link">Sign out</button></form></header>
<main>
<section class="tiles" id="tiles"></section>
<section class="grid2">
 <div class="card"><h2>The Great Sea</h2><div id="chart"></div><p class="hint">Players at sea. Grey: connection lost (kept for resume).</p></div>
 <div class="card"><h2>Places</h2><ul id="places" class="places"></ul></div>
</section>
<section class="card"><div class="row"><h2>Players</h2><input id="filter" placeholder="Filter by name, code or place" aria-label="Filter"></div>
<div class="scroll"><table id="players"><thead><tr><th>Name</th><th>Friend code</th><th>Place</th><th>Party</th><th>Ping</th><th>Device</th><th>Online</th><th>PvP</th></tr></thead><tbody></tbody></table></div></section>
<section class="card"><h2>Parties</h2><div class="scroll"><table id="parties"><thead><tr><th>Id</th><th>Leader</th><th>Members</th><th>Names</th><th>State</th></tr></thead><tbody></tbody></table></div></section>
<p class="foot" id="foot"></p>
</main><script src="/static/app.js"></script>"""


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "wwhd-status"
    sys_version = ""
    protocol_version = "HTTP/1.1"
    timeout = 15
    app = None  # set by serve()

    # -- helpers

    def client_ip(self):
        peer = self.client_address[0]
        try:
            trusted = ipaddress.ip_address(peer) in self.app["trusted"]
        except ValueError:
            trusted = False
        if trusted:
            xff = self.headers.get("X-Forwarded-For", "")
            if xff:
                last = xff.split(",")[-1].strip()
                try:
                    return str(ipaddress.ip_address(last))
                except ValueError:
                    pass
        return peer

    def cookie_name(self):
        return "__Host-wwhd" if self.app["secure"] else "wwhd"

    def session_token(self):
        raw = self.headers.get("Cookie", "")
        for part in raw.split(";"):
            k, _, v = part.strip().partition("=")
            if k == self.cookie_name():
                return v
        return None

    def session(self):
        tok = self.session_token()
        sess = self.app["sessions"].get(tok)
        if sess:
            rec = self.app["users"].get(sess["user"])  # a deleted user or a new password ends the session
            if not rec or not hmac.compare_digest(rec["hash"], sess["hash"]):
                self.app["sessions"].drop(tok)
                return None
        return sess

    def send(self, code, body, ctype="text/html; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        for k, v in SECURITY_HEADERS.items():
            self.send_header(k, v)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, where, headers=None):
        h = {"Location": where}
        h.update(headers or {})
        self.send(303, "", headers=h)

    def same_origin(self):
        """POSTs only from our own pages (with SameSite=Strict cookies, this stops cross-site form posts)"""
        site = self.headers.get("Sec-Fetch-Site")
        if site is not None:  # every current browser sends it; it can't be set by a page
            return site == "same-origin"
        origin = self.headers.get("Origin")
        if not origin or origin == "null":
            origin = self.headers.get("Referer")
        host = self.headers.get("Host", "")
        if not origin or not host:
            return False
        return urllib.parse.urlsplit(origin).netloc == host

    def read_form(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if n < 0 or n > MAX_BODY:
            return None
        if not self.headers.get("Content-Type", "").startswith("application/x-www-form-urlencoded"):
            return None
        try:
            return urllib.parse.parse_qs(self.rfile.read(n).decode("utf-8", "replace"), max_num_fields=8)
        except ValueError:  # more fields than any of our forms has
            return None

    def log_message(self, fmt, *args):
        pass  # requests are not logged (player data stays out of logs); logins are, below

    def page(self, title, body, cls=""):
        return PAGE % {"title": html.escape(title), "body": body, "cls": cls}

    def login_page(self, code=200, msg=""):
        m = '<p class="msg">%s</p>' % html.escape(msg) if msg else ""
        self.send(code, self.page("Sign in", LOGIN % {"msg": m}, "plain"))

    # -- routes

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/login":
            if self.session():
                return self.redirect("/")
            return self.login_page()
        if path == "/healthz":
            ok = self.app["status"].get().get("reachable")
            return self.send(200 if ok else 503, "ok\n" if ok else "down\n", "text/plain; charset=utf-8")
        if path.startswith("/static/"):
            name = path[len("/static/"):]
            if name in ("app.css", "app.js"):
                with open(os.path.join(STATIC, name), "rb") as f:
                    body = f.read()
                return self.send(200, body, "text/css; charset=utf-8" if name.endswith(".css")
                                 else "text/javascript; charset=utf-8")
            return self.send(404, "not found\n", "text/plain; charset=utf-8")
        sess = self.session()
        if path == "/":
            if not sess:
                return self.redirect("/login")
            return self.send(200, self.page("Server status", DASH % {
                "csrf": html.escape(sess["csrf"]), "user": html.escape(sess["user"])}))
        if path == "/api/status":
            if not sess:
                return self.send(401, '{"error":"sign in"}', "application/json")
            d = dict(self.app["status"].get())
            d["sea_names"] = self.app["status"].sea
            return self.send(200, json.dumps(d, separators=(",", ":")), "application/json")
        return self.send(404, "not found\n", "text/plain; charset=utf-8")

    def do_POST(self):
        path = urllib.parse.urlsplit(self.path).path
        if not self.same_origin():
            return self.send(403, "forbidden\n", "text/plain; charset=utf-8")
        form = self.read_form()
        if form is None:
            return self.send(400, "bad request\n", "text/plain; charset=utf-8")
        f = lambda k: (form.get(k) or [""])[0]
        if path == "/login":
            return self.login(f("user").strip(), f("password"))
        if path == "/logout":
            sess = self.session()
            if sess and hmac.compare_digest(f("csrf"), sess["csrf"]):
                self.app["sessions"].drop(self.session_token())
            return self.redirect("/login", {"Set-Cookie": self.cookie(None)})
        return self.send(404, "not found\n", "text/plain; charset=utf-8")

    def cookie(self, tok):
        sec = "; Secure" if self.app["secure"] else ""
        if tok is None:
            return "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict%s" % (self.cookie_name(), sec)
        return "%s=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Strict%s" % (self.cookie_name(), tok, SESSION_MAX_S, sec)

    def login(self, user, pw):
        ip = self.client_ip()
        thr = self.app["throttle"]
        key = user.lower()[:32]
        wait = thr.wait_s(ip, key)
        if wait:
            log("login throttled user=%s ip=%s" % (key or "-", ip))
            return self.login_page(429, "Too many failed sign-ins. Try again in %d minutes." % ((wait + 59) // 60))
        rec = self.app["users"].get(user) if NAME_RE.match(user) and 0 < len(pw) <= 256 else None
        ok = check_password(pw, rec["hash"] if rec else DUMMY_HASH)
        if not (rec and ok):
            thr.fail(ip, key)
            log("login failed user=%s ip=%s" % (key if NAME_RE.match(key) else "-", ip))
            time.sleep(0.5)
            return self.login_page(401, "Wrong user or password.")
        thr.ok(ip, key)
        log("login ok user=%s ip=%s" % (user, ip))
        tok = self.app["sessions"].new(user, rec["hash"])
        return self.redirect("/", {"Set-Cookie": self.cookie(tok)})


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 64

    def __init__(self, addr, handler):
        if ":" in addr[0]:
            self.address_family = socket.AF_INET6
        super().__init__(addr, handler)


def serve(a):
    global DUMMY_HASH
    DUMMY_HASH = hash_password(secrets.token_urlsafe(16))
    users = Users(a.users)
    if not users.all():
        log("no users yet: add one with  %s adduser NAME --users %s" % (sys.argv[0], a.users))
    trusted = [ipaddress.ip_network(t.strip()) for t in a.trusted_proxy.split(",") if t.strip()]

    class Trusted:
        def __contains__(self, ip):
            return any(ip in n for n in trusted)

    Handler.app = {"users": users, "sessions": Sessions(), "throttle": Throttle(), "status": Status(a.socket),
                   "secure": not a.insecure_cookies, "trusted": Trusted()}
    if a.insecure_cookies and a.bind not in ("127.0.0.1", "::1", "localhost"):
        log("warning: --insecure-cookies on a non-loopback address: passwords and sessions travel unencrypted")
    httpd = Server((a.bind, a.port), Handler)
    log("listening on %s:%d (status socket %s)" % (a.bind, a.port, a.socket))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description="the online server's status page behind a login")
    ap.add_argument("command", choices=["serve", "adduser", "passwd", "deluser", "users"])
    ap.add_argument("name", nargs="?")
    ap.add_argument("--users", default=os.environ.get("WWHD_STATUS_USERS", "status-users.json"))
    ap.add_argument("--socket", default=os.environ.get("WWHD_STATUS_SOCKET", "wwhd-online-status.sock"))
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8780)
    ap.add_argument("--trusted-proxy", default="127.0.0.1/32,::1/128",
                    help="addresses whose X-Forwarded-For is believed (the reverse proxy)")
    ap.add_argument("--insecure-cookies", action="store_true",
                    help="cookies without Secure (only for a local test over plain http)")
    a = ap.parse_args()
    if a.command == "serve":
        return serve(a)
    users = Users(a.users)
    u = users.all()
    if a.command == "users":
        for n, r in sorted(u.items()):
            print(n, time.strftime("%Y-%m-%d", time.gmtime(r.get("created", 0))))
        return
    if not a.name or not NAME_RE.match(a.name):
        sys.exit("a user name: letters, digits, _ . - (up to 32)")
    if a.command == "deluser":
        if u.pop(a.name, None) is None:
            sys.exit("no such user")
        users.save(u)
        print("deleted %s (their open sessions end at once)" % a.name)
        return
    if a.command == "adduser" and a.name in u:
        sys.exit("already there; use passwd")
    if a.command == "passwd" and a.name not in u:
        sys.exit("no such user; use adduser")
    pw = ask_password()
    u[a.name] = {"hash": hash_password(pw), "created": u.get(a.name, {}).get("created", int(time.time()))}
    users.save(u)
    print("saved %s" % a.name)


if __name__ == "__main__":
    main()
