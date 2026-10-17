#!/usr/bin/env bash
#
# SoulX Android Emulator - package SoulX.app and its .dmg
#
# Builds the soulx CLI (Release) and the SwiftUI launcher, assembles build/dist/SoulX.app, signs it ad hoc and makes
# build/dist/SoulX-<version>.dmg and a .zip. SoulX is deliberately not signed with a Developer ID nor notarized:
# on first open people allow it in System Settings > Privacy & Security ("Open Anyway"). Apple Silicon still needs
# an ad hoc signature on every executable, which involves no certificate.
#
#   SoulX.app/Contents/MacOS/SoulX       the launcher (what opens when you double-click)
#   SoulX.app/Contents/Helpers/soulx     the CLI; it also runs SoulX's window and the MCP server
#   SoulX.app/Contents/Resources/        the icon
#
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/package_macos.sh [--skip-build] [--no-dmg]

  --skip-build   use the existing build/macos/soulx and launcher build
  --no-dmg       stop after SoulX.app (and its .zip)
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

skip_build=0
make_dmg=1
for arg in "$@"; do
    case "$arg" in
        --skip-build) skip_build=1 ;;
        --no-dmg)     make_dmg=0 ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "error: unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
cd "$repo_root"

version="$(sed -n 's/^#define SOULX_VERSION_STRING "\(.*\)"$/\1/p' source/core/soulx_android.h)"
[[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "cannot read SOULX_VERSION_STRING from source/core/soulx_android.h"
build_number="$(git rev-list --count HEAD 2>/dev/null || echo 1)"

dist="build/dist"
app="$dist/SoulX.app"

# --- Build ---------------------------------------------------------------------
if [[ "$skip_build" == 0 ]]; then
    ./scripts/build_macos.sh
    echo "=== Building the launcher (release) ==="
    (cd launcher && swift build -c release --arch arm64)
fi
launcher_bin="$(cd launcher && swift build -c release --arch arm64 --show-bin-path)/SoulXLauncher"
[[ -x build/macos/soulx ]] || die "build/macos/soulx is missing (run without --skip-build)"
[[ -x "$launcher_bin" ]] || die "the launcher is not built: $launcher_bin"

# --- Assemble ------------------------------------------------------------------
echo "=== Assembling $app ($version, build $build_number) ==="
rm -rf -- "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Helpers" "$app/Contents/Resources"
cp "$launcher_bin" "$app/Contents/MacOS/SoulX"
cp build/macos/soulx "$app/Contents/Helpers/soulx"

iconset="$dist/SoulX.iconset"
rm -rf -- "$iconset"
swift scripts/make_icon.swift "$iconset"
iconutil -c icns -o "$app/Contents/Resources/SoulX.icns" "$iconset"
rm -rf -- "$iconset"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>     <string>en</string>
    <key>CFBundleDisplayName</key>           <string>SoulX</string>
    <key>CFBundleExecutable</key>            <string>SoulX</string>
    <key>CFBundleIconFile</key>              <string>SoulX</string>
    <key>CFBundleIdentifier</key>            <string>io.github.t2o0n321.soulx</string>
    <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
    <key>CFBundleName</key>                  <string>SoulX</string>
    <key>CFBundlePackageType</key>           <string>APPL</string>
    <key>CFBundleShortVersionString</key>    <string>$version</string>
    <key>CFBundleVersion</key>               <string>$build_number</string>
    <key>CFBundleLocalizations</key>
    <array><string>en</string><string>zh-Hant</string><string>zh-Hans</string></array>
    <key>LSApplicationCategoryType</key>     <string>public.app-category.games</string>
    <key>LSArchitecturePriority</key>        <array><string>arm64</string></array>
    <key>LSMinimumSystemVersion</key>        <string>14.0</string>
    <key>NSHighResolutionCapable</key>       <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
</dict>
</plist>
EOF
plutil -lint "$app/Contents/Info.plist" >/dev/null

# --- Sign (ad hoc) -----------------------------------------------------------------
# Inside out: the helper first, then the bundle.
echo "=== Signing (ad hoc) ==="
codesign --force --timestamp=none --sign - "$app/Contents/Helpers/soulx"
codesign --force --timestamp=none --sign - "$app"
codesign --verify --strict --deep "$app"

# --- Archives ------------------------------------------------------------------
zip="$dist/SoulX-$version.zip"
rm -f -- "$zip"
ditto -c -k --keepParent "$app" "$zip"
echo "  $zip"

if [[ "$make_dmg" == 1 ]]; then
    dmg="$dist/SoulX-$version.dmg"
    stage="$dist/dmg"
    rm -rf -- "$stage" "$dmg"
    mkdir -p "$stage"
    cp -R "$app" "$stage/"
    ln -s /Applications "$stage/Applications"
    hdiutil create -quiet -volname "SoulX $version" -srcfolder "$stage" -fs HFS+ -format UDZO "$dmg"
    rm -rf -- "$stage"
    echo "  $dmg"
fi

(cd "$dist" && shasum -a 256 SoulX-"$version".* > "SoulX-$version.sha256")
echo "=== Done: $app ==="
