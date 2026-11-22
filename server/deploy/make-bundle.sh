#!/bin/sh
# Packs what a VPS needs into one tarball (fork; docs/online-server-vps.md): the server's sources (server/ +
# runtime/src/online/proto), the status page and this deploy folder. No game data, no build output, no keys.
#   server/deploy/make-bundle.sh [out.tar.gz]      then: scp it to the VPS, untar, sudo ./deploy/install.sh
set -eu
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
out=${1:-wwhd-online-bundle-$(git -C "$root" rev-parse --short HEAD).tar.gz}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
d="$tmp/wwhd-online"
mkdir -p "$d/runtime/src/online"
cp -R "$root/server" "$d/server"
cp -R "$root/runtime/src/online/proto" "$d/runtime/src/online/proto"
cp "$root/runtime/src/online/fork_proto.h" "$d/runtime/src/online/"  # the fork words (server.cpp, fork_quests.cpp)
cp "$root/docs/online-server.md" "$root/docs/online-server-vps.md" "$d/" 2>/dev/null || true
rm -rf "$d/server/status/__pycache__"
find "$d" -name '*.key' -o -name 'users.json' -o -name 'status-users.json' | xargs -r rm -f
mv "$d/server/deploy" "$d/deploy"
tar -C "$tmp" -czf "$out" wwhd-online
echo "$out"
