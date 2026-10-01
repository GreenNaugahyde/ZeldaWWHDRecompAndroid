#!/bin/sh
# syntax-check vendored cemu files; print unique error messages
V=runtime/third_party/cemu
for f in "$@"; do
  clang++ -std=c++20 -fsyntax-only -ferror-limit=200 -include $V/cemu_shim.h -I$V -Iruntime/third_party/metal-cpp -Iruntime/third_party/fmt/include "$f" 2>&1 | grep -E "error:" | sed 's/^[^ ]*: error: //'
done | sort | uniq -c | sort -rn | head -${N:-40}
