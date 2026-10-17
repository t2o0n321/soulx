#!/usr/bin/env bash
#
# SoulX Android Emulator - prints the GitHub Release text for a version: its CHANGELOG.md section, then how to
# install SoulX (it is not notarized, so the first open needs "Open Anyway").
#
# Usage: scripts/release_notes.sh VERSION
#
set -euo pipefail

version="${1:?usage: release_notes.sh VERSION}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The "## <version>" section of CHANGELOG.md, without its heading (empty when there is none).
section="$(awk -v v="$version" '
    /^## / { if (found) exit; found = ($2 == v || $2 == "v" v || $2 == "[" v "]") ; next }
    found { print }
' "$repo_root/CHANGELOG.md" 2>/dev/null || true)"

if [[ -n "${section//[[:space:]]/}" ]]; then
    printf '%s\n\n' "$section"
fi

cat <<EOF
## Install

Requires a Mac with Apple Silicon (M1 or later) and macOS 14 or later.

1. Download **SoulX-$version.dmg**, open it and drag **SoulX** into **Applications**.
2. Open SoulX from Applications. SoulX is not notarized by Apple, so macOS stops the first open:
   click **Done** (not "Move to Trash"), then go to **System Settings > Privacy & Security**, scroll down to
   "SoulX was blocked…" and click **Open Anyway**, then confirm. You do this once.
3. SoulX downloads its Android 17 runtime on first use (about 2.7 GB).

## 安裝

需要 Apple Silicon（M1 或更新）的 Mac，macOS 14 以上。

1. 下載 **SoulX-$version.dmg**，打開後把 **SoulX** 拖到「應用程式」。
2. 從「應用程式」打開 SoulX。SoulX 沒有經過 Apple 公證，第一次打開會被 macOS 擋下：按「完成」（不要按「丟到垃圾桶」），
   到「系統設定 > 隱私權與安全性」，往下找到「已阻擋 SoulX…」，按「強制打開」並確認。只需要做一次。
3. 第一次使用時 SoulX 會下載 Android 17 執行環境（約 2.7 GB）。

Checksums: \`SoulX-$version.sha256\`.
EOF
