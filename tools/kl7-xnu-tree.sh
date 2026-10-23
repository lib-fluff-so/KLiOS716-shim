#!/usr/bin/env bash
# Build the patched XNU source tree for KL7 locally (no compile; macOS-only).
# Usage: tools/kl7-xnu-tree.sh /path/to/ios-sweet-clone OUT_DIR [last-sweet-patch-number]
#   e.g. tools/kl7-xnu-tree.sh ~/src/ios-sweet /tmp/xnu-kl7        # all sweet patches
#        tools/kl7-xnu-tree.sh ~/src/ios-sweet /tmp/xnu-kl7 18     # only sweet 0001-0018 (0102 then SKIPs: it is
#                                                                 # generated against the full series)
# Source: apple-oss-distributions/xnu tag xnu-8796.141.3 (macOS 13.5, what the
# ios-sweet CI builds). Patches that need staged HFS/libpthread/BearSSL sources
# (0019/0020/0026 scripts) will skip here exactly like they would without CI.
set -eu
shopt -s nullglob
SWEET=$(cd "$1" && pwd); mkdir -p "$2"; OUT=$(cd "$2" && pwd); LAST=${3:-81}
KL7=$(cd "$(dirname "$0")/.." && pwd)/xnu/patches-kl7
TAG=xnu-8796.141.3
[ -d "$OUT/.git" ] || git clone -q --depth 1 --branch "$TAG" https://github.com/apple-oss-distributions/xnu "$OUT"
cd "$OUT"
git checkout -q . && git clean -fdq
for f in "$SWEET"/xnu/patches/*.patch "$KL7"/*.patch; do
  b=$(basename "$f"); n=${b%%-*}
  case $b in 01*) ;; *) [ "$((10#$n))" -le "$LAST" ] || continue ;; esac
  if git apply --whitespace=nowarn --check "$f" 2>/dev/null; then git apply --whitespace=nowarn "$f"; echo "ok   $b"
  else echo "SKIP $b"; fi
done
