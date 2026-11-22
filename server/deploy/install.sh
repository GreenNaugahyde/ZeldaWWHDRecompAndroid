#!/bin/sh
# Installs (or updates) the online server and its status page on a Debian / Ubuntu VPS (fork;
# docs/online-server-vps.md). Run as root from the unpacked bundle:  sudo ./deploy/install.sh
# It builds the server from source, makes the users wwhd and wwhd-status, installs the systemd units and starts
# them. It never touches the firewall, SSH, Caddy or fail2ban: the guide has those steps, to do with care.
# An existing /etc/wwhd-online/server.toml, server key, bans and status logins are kept.
set -eu
[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
here=$(cd "$(dirname "$0")/.." && pwd)
[ -d "$here/server" ] && [ -d "$here/deploy" ] || { echo "run from the unpacked bundle (make-bundle.sh)"; exit 1; }

echo "== packages"
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q g++ cmake ninja-build python3

echo "== build"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S "$here/server" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$b" wwhd-online-server wwhd_online_server_test wwhd_fork_quests_test
echo "== tests"
(cd "$b" && ./wwhd_online_server_test . >/dev/null && ./wwhd_fork_quests_test . >/dev/null) || { echo "server tests failed: not installed"; exit 1; }

echo "== users"
getent group wwhd >/dev/null || groupadd --system wwhd
id wwhd >/dev/null 2>&1 || useradd --system --gid wwhd --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin wwhd
getent group wwhd-status >/dev/null || groupadd --system wwhd-status
id wwhd-status >/dev/null 2>&1 || useradd --system --gid wwhd-status --home-dir /nonexistent --no-create-home --shell /usr/sbin/nologin wwhd-status

echo "== files"
install -d -m 0755 /opt/wwhd /opt/wwhd/bin /opt/wwhd/status /opt/wwhd/status/static
install -m 0755 "$b/wwhd-online-server" /opt/wwhd/bin/wwhd-online-server
install -m 0644 "$here/server/status/wwhd_status.py" "$here/server/status/names.json" /opt/wwhd/status/
install -m 0644 "$here/server/status/static/app.css" "$here/server/status/static/app.js" /opt/wwhd/status/static/
install -m 0755 "$here/deploy/wwhd-admin" "$here/deploy/wwhd-status-user" /usr/local/sbin/
install -d -m 0750 -o root -g wwhd /etc/wwhd-online
if [ ! -f /etc/wwhd-online/server.toml ]; then
  install -m 0640 -o root -g wwhd "$here/deploy/server.vps.toml" /etc/wwhd-online/server.toml
  echo "   new /etc/wwhd-online/server.toml: set server.contact (and name / rules) before players join"
else
  echo "   kept /etc/wwhd-online/server.toml (the new template: deploy/server.vps.toml)"
fi
install -m 0644 "$here/deploy/wwhd-online.service" "$here/deploy/wwhd-status.service" /etc/systemd/system/

echo "== services"
systemctl daemon-reload
systemctl enable wwhd-online.service wwhd-status.service >/dev/null
systemctl restart wwhd-online.service
sleep 1
systemctl restart wwhd-status.service
systemctl --no-pager --lines=5 status wwhd-online.service || true
echo
echo "Done. Next (docs/online-server-vps.md):"
echo "  sudo wwhd-status-user adduser NAME          a login for the status page"
echo "  sudo wwhd-admin status                      the server answers"
echo "  firewall, SSH keys only, Caddy + TLS, fail2ban: the guide's steps 3-6"
