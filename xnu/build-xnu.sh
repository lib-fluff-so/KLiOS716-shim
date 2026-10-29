#!/bin/bash
# Local replica of .github/workflows/xnu.yml (ios-sweet), no GitHub Actions needed.
# Usage: MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET bash ~/kbuild/build-xnu.sh
set -euo pipefail
export PATH=/usr/local/bin:$PATH
export MACOS_VERSION="${MACOS_VERSION:-13.5}"
export KERNEL_CONFIG="${KERNEL_CONFIG:-DEVELOPMENT}"
export MACHINE_CONFIG="${MACHINE_CONFIG:-SWEET}"
export MAKEFLAGS="${MAKEFLAGS:--k}"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
cd "$HOME/kbuild"

# 1. fresh dxb checkout at the pinned SHA
rm -rf dxb
git init -q dxb
git -C dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
git -C dxb checkout -q FETCH_HEAD

# 2. our patches next to blacktop's
cp ios-sweet/xnu/patches/*.patch ios-sweet/xnu/patches/*.sh dxb/patches/
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' dxb/build.sh

# 3. build
cd dxb
bash build.sh

# 4. collect artifacts like the workflow does
mkdir -p "$HOME/kbuild/out"
rm -rf "$HOME/kbuild/out"/*
cp fakeroot/System/Library/Kernels/kernel.* "$HOME/kbuild/out/" 2>/dev/null || true
rm -rf "$HOME/kbuild/out/"*.link
( cd "$HOME/kbuild/out" && shasum -a 256 kernel.* > SHA256SUMS )
echo "ARTIFACTS:"
ls -l "$HOME/kbuild/out"
cat "$HOME/kbuild/out/SHA256SUMS"
