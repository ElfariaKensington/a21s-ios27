#!/usr/bin/env bash
# Reproducibly apply the complete Galaxy A21s (SM-A217F / Exynos 850) iOS 27 XNU 12377.121.6 patch
set -euo pipefail
B="$(cd "$(dirname "$0")" && pwd)"
XNU="${1:-$B/work/build/dxb-12377/xnu}"
git -C "$XNU" checkout -- .
git -C "$XNU" apply "$B/out/a21s-xnu-12377.patch"
echo "[ok] Applied $B/out/a21s-xnu-12377.patch to $XNU"
